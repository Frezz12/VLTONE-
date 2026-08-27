#include "RecordingEngine.hpp"
#include "platform/Log.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

namespace audio {

namespace {

/// WAVE_FORMAT_IEEE_FLOAT header.
///
/// Float WAV requires an 18-byte `fmt ` chunk (with the `cbSize` extension
/// field) followed by a `fact` chunk. Written with the bare 16-byte PCM `fmt `
/// chunk — as this did — strict readers refuse to open the file, so the app
/// could not read back its own recordings or exports.
#pragma pack(push, 1)
struct WAVHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t fileSize = 0;
    char wave[4] = {'W', 'A', 'V', 'E'};

    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 18;
    uint16_t audioFormat = 3;   // WAVE_FORMAT_IEEE_FLOAT
    uint16_t numChannels = 2;
    uint32_t sampleRate = 44100;
    uint32_t byteRate = 0;
    uint16_t blockAlign = 0;
    uint16_t bitsPerSample = 32;
    uint16_t extensionSize = 0;

    char fact[4] = {'f', 'a', 'c', 't'};
    uint32_t factSize = 4;
    uint32_t sampleLength = 0;  // frames per channel

    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)

static_assert(sizeof(WAVHeader) == 58, "WAV header must be tightly packed");

/// The largest number of frames a RIFF header can describe, for this layout.
/// RIFF sizes are 32-bit, so a take longer than this cannot be written as a
/// plain WAV however long the disk would allow — around three hours of stereo
/// float at 48 kHz.
uint64_t maxWavFrames(uint16_t blockAlign) {
    if (blockAlign == 0) return 0;
    const uint64_t room = 0xFFFFFFFFull - (sizeof(WAVHeader) - 8);
    return room / blockAlign;
}

WAVHeader makeHeader(uint16_t channels, uint32_t sampleRate, uint64_t frames) {
    WAVHeader header;
    header.numChannels = channels;
    header.sampleRate = sampleRate;
    header.bitsPerSample = 32;
    header.blockAlign = channels * (header.bitsPerSample / 8);
    header.byteRate = sampleRate * header.blockAlign;
    // Computed in 64 bits and clamped, not truncated. Truncating produced a
    // header describing a few minutes of a three-hour take: the file on disk
    // was complete and every reader in the world showed a fraction of it,
    // which is indistinguishable from having lost the recording.
    const uint64_t clamped = std::min<uint64_t>(frames, maxWavFrames(header.blockAlign));
    header.sampleLength = static_cast<uint32_t>(clamped);
    header.dataSize = static_cast<uint32_t>(clamped * header.blockAlign);
    // Everything after the eight-byte RIFF/size prefix.
    header.fileSize = static_cast<uint32_t>(sizeof(WAVHeader) - 8) + header.dataSize;
    return header;
}

/// Round up to the next power of two so the ring can mask instead of divide.
size_t nextPowerOfTwo(size_t value) {
    size_t result = 1;
    while (result < value) result <<= 1;
    return result;
}

} // namespace

AudioRecorder::AudioRecorder() = default;
AudioRecorder::~AudioRecorder() { shutdown(); }

Result AudioRecorder::initialize(SampleRate sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return Result::ok();

    m_sampleRate = sampleRate;
    m_fileChannels = static_cast<ChannelCount>(std::max(1u, channels));
    m_session.sampleRate = sampleRate;
    m_session.channelCount = m_fileChannels;

    // Roughly four seconds of headroom, so a busy disk cannot cost us audio.
    const auto samples = static_cast<size_t>(sampleRate) * m_fileChannels * 4;
    m_ringCapacity = nextPowerOfTwo(std::max<size_t>(samples, 1 << 16));
    m_ring.assign(m_ringCapacity, 0.0f);
    m_writeIndex.store(0);
    m_readIndex.store(0);

    m_initialized = true;
    return Result::ok();
}

void AudioRecorder::shutdown() {
    if (isRecording()) {
        stopRecording();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ring.clear();
    m_ringCapacity = 0;
    m_initialized = false;
}

void AudioRecorder::setInputChannels(ChannelCount firstChannel,
                                     ChannelCount count) {
    m_inputFirstChannel = firstChannel;
    m_inputChannelCount = std::max<ChannelCount>(count, 1);
}

std::string AudioRecorder::makeRecordingPath(TrackID trackID) const {
    namespace fs = std::filesystem;

    fs::path directory = m_recordPath.empty()
        ? fs::temp_directory_path() / "VLT Studio Pro Recordings"
        : daw::platform::pathFromUtf8(m_recordPath);

    std::error_code ec;
    fs::create_directories(directory, ec);

    const auto now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &parts);

    char name[96];
    std::snprintf(name, sizeof(name), "Track %u %s.wav",
                  static_cast<unsigned>(trackID), stamp);
    return daw::platform::pathToUtf8(directory / name);
}

Result AudioRecorder::startRecording(TrackID trackID, TimeSamples startSample) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_initialized) return Result::fail(EngineError::NotInitialized);
        if (m_recording.load()) return Result::fail(EngineError::AlreadyRecording);

        m_session.state = RecordingSession::State::Preparing;
        m_session.trackID = trackID;
        m_session.startSample = startSample;
        m_session.recordedSamples = 0;
        m_session.sampleRate = m_sampleRate;
        m_session.channelCount = m_fileChannels;
        m_session.filePath = makeRecordingPath(trackID);
    }

    // Reserve space for the header; it is rewritten with the real sizes when
    // the recording stops.
    {
        std::ofstream file(daw::platform::pathFromUtf8(m_session.filePath),
                           std::ios::binary | std::ios::trunc);
        if (!file) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_session.state = RecordingSession::State::Idle;
            m_session.filePath.clear();
            return Result::fail(EngineError::FileWriteError,
                                "Could not create the recording file");
        }
        const auto header = makeHeader(static_cast<uint16_t>(m_fileChannels),
                                       static_cast<uint32_t>(m_sampleRate), 0);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }

    m_writeIndex.store(0, std::memory_order_relaxed);
    m_readIndex.store(0, std::memory_order_relaxed);
    m_droppedFrames.store(0, std::memory_order_relaxed);
    m_recordedFrames.store(0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_session.state = RecordingSession::State::Recording;
    }
    m_recording.store(true, std::memory_order_release);

    m_writerRunning.store(true, std::memory_order_release);
    m_writerThread = std::thread([this] { writerLoop(); });

    return Result::ok();
}

Result AudioRecorder::stopRecording() {
    if (!m_recording.exchange(false, std::memory_order_acq_rel)) {
        return Result::fail(EngineError::NotRecording);
    }

    // A callback may have observed the old flag immediately before the
    // exchange. Wait until that producer has published its final ring write
    // before telling the writer to drain and exit.
    while (m_processInFlight.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }

    // Let the writer drain whatever is still in the ring, then join.
    m_writerRunning.store(false, std::memory_order_release);
    m_writerSignal.notify_all();
    if (m_writerThread.joinable()) {
        m_writerThread.join();
    }

    RecordingSession completed;
    RecordingCompleteCallback completeCallback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_session.recordedSamples =
            m_recordedFrames.load(std::memory_order_relaxed);
        m_session.state = RecordingSession::State::Stopped;
        completed = m_session;
        m_session.state = RecordingSession::State::Idle;
        completeCallback = m_completeCallback;
    }

    if (completeCallback) {
        completeCallback(completed);
    }
    return Result::ok();
}

void AudioRecorder::process(const AudioBuffer& input, BufferSize numFrames) {
    if (!m_recording.load(std::memory_order_acquire)) return;
    m_processInFlight.fetch_add(1, std::memory_order_acq_rel);
    struct ProcessGuard {
        std::atomic<std::uint32_t>& count;
        ~ProcessGuard() { count.fetch_sub(1, std::memory_order_release); }
    } guard{m_processInFlight};
    if (!m_recording.load(std::memory_order_acquire)) return;
    if (m_ringCapacity == 0 || !input.isValid()) return;

    const BufferSize frames = std::min(numFrames, input.numFrames());
    if (frames == 0) return;

    const size_t mask = m_ringCapacity - 1;
    const size_t write = m_writeIndex.load(std::memory_order_relaxed);
    const size_t read = m_readIndex.load(std::memory_order_acquire);
    const size_t used = (write - read) & mask;
    const size_t free = mask - used;

    const size_t needed = static_cast<size_t>(frames) * m_fileChannels;
    if (needed > free) {
        // Better to drop a block than to block the audio thread.
        m_droppedFrames.fetch_add(frames, std::memory_order_relaxed);
        return;
    }

    size_t cursor = write;
    for (BufferSize frame = 0; frame < frames; ++frame) {
        for (ChannelCount ch = 0; ch < m_fileChannels; ++ch) {
            // Mono sources are duplicated across the file's channels.
            const ChannelCount sourceCh = m_inputFirstChannel +
                (m_inputChannelCount >= 2 ? ch : 0);
            const float* data = sourceCh < input.numChannels()
                ? input.getChannel(sourceCh) : nullptr;
            m_ring[cursor] = data ? data[frame] : 0.0f;
            cursor = (cursor + 1) & mask;
        }
    }
    m_writeIndex.store(cursor, std::memory_order_release);

    m_recordedFrames.fetch_add(frames, std::memory_order_relaxed);
}

void AudioRecorder::writerLoop() {
    std::ofstream file(daw::platform::pathFromUtf8(m_session.filePath),
                       std::ios::binary | std::ios::in | std::ios::out);
    if (!file) return;
    file.seekp(0, std::ios::end);

    const size_t mask = m_ringCapacity - 1;
    uint64_t samplesWritten = 0;
    std::vector<float> chunk(4096);

    const auto drain = [&] {
        while (true) {
            const size_t write = m_writeIndex.load(std::memory_order_acquire);
            size_t read = m_readIndex.load(std::memory_order_relaxed);
            size_t available = (write - read) & mask;
            if (available == 0) break;

            const size_t count = std::min(available, chunk.size());
            for (size_t i = 0; i < count; ++i) {
                chunk[i] = m_ring[(read + i) & mask];
            }
            file.write(reinterpret_cast<const char*>(chunk.data()),
                       static_cast<std::streamsize>(count * sizeof(float)));
            samplesWritten += count;
            m_readIndex.store((read + count) & mask, std::memory_order_release);
        }
    };

    while (m_writerRunning.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lock(m_writerMutex);
            // Polling avoids a potentially blocking condition-variable notify
            // from the realtime producer. Five milliseconds is well inside
            // the ring's four seconds of headroom.
            m_writerSignal.wait_for(lock, std::chrono::milliseconds(5));
        }
        drain();
    }
    // Final pass for anything the audio thread pushed while we were shutting
    // down.
    drain();
    file.flush();
    file.close();

    finalizeFile(samplesWritten / std::max<ChannelCount>(m_fileChannels, 1));
}

void AudioRecorder::finalizeFile(uint64_t framesWritten) {
    std::fstream file(daw::platform::pathFromUtf8(m_session.filePath),
                      std::ios::binary | std::ios::in | std::ios::out);
    if (!file) return;

    const auto blockAlign = static_cast<uint16_t>(m_fileChannels * 4);
    if (framesWritten > maxWavFrames(blockAlign)) {
        DAW_LOG_ERROR("[Recording] take is longer than a WAV header can "
                      "describe (%llu frames); the file says %llu",
                      static_cast<unsigned long long>(framesWritten),
                      static_cast<unsigned long long>(maxWavFrames(blockAlign)));
    }
    const auto header = makeHeader(static_cast<uint16_t>(m_fileChannels),
                                   static_cast<uint32_t>(m_sampleRate),
                                   framesWritten);
    file.seekp(0, std::ios::beg);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.close();
}

void AudioRecorder::setRecordingCompleteCallback(
    RecordingCompleteCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_completeCallback = std::move(cb);
}

RecordingSession AudioRecorder::session() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    RecordingSession copy = m_session;
    if (copy.state == RecordingSession::State::Recording) {
        copy.recordedSamples = m_recordedFrames.load(std::memory_order_relaxed);
    }
    return copy;
}

Result AudioRecorder::writeWAVFile(const std::string& path,
                                    const AudioBuffer& buffer,
                                    SampleRate rate) {
    if (!buffer.isValid()) {
        return Result::fail(EngineError::InvalidArgument, "Empty buffer");
    }

    const auto channels = static_cast<uint16_t>(buffer.numChannels());
    const auto frames = static_cast<uint32_t>(buffer.numFrames());
    const auto header = makeHeader(channels, static_cast<uint32_t>(rate), frames);

    std::ofstream file(daw::platform::pathFromUtf8(path),
                       std::ios::binary | std::ios::trunc);
    if (!file) {
        return Result::fail(EngineError::FileWriteError);
    }

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Interleave through a staging buffer rather than one ofstream::write per
    // sample, which for a five-minute bounce was millions of calls.
    std::vector<float> interleaved(static_cast<size_t>(channels) * 4096);
    uint32_t frame = 0;
    while (frame < frames) {
        const uint32_t block = std::min<uint32_t>(4096, frames - frame);
        for (uint32_t f = 0; f < block; ++f) {
            for (uint16_t ch = 0; ch < channels; ++ch) {
                const float* data = buffer.getChannel(ch);
                interleaved[f * channels + ch] = data ? data[frame + f] : 0.0f;
            }
        }
        file.write(reinterpret_cast<const char*>(interleaved.data()),
                   static_cast<std::streamsize>(block * channels * sizeof(float)));
        frame += block;
    }

    file.close();
    return file.good() || !file.fail()
        ? Result::ok()
        : Result::fail(EngineError::FileWriteError);
}

} // namespace audio
