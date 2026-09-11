#include "RecordingEngine.hpp"
#include <bit>
#include "platform/Log.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

enum class ExclusiveCreateResult { Created, AlreadyExists, Failed };

/// Reserve a recorder path and write its initial header in one exclusive-create
/// operation. A timestamp (even with sub-second precision) is not an identity:
/// two armed tracks begin in the same callback interval, and two application
/// processes may share a recording directory. The operating system is the final
/// authority that a candidate belongs to exactly one recorder.
ExclusiveCreateResult createExclusiveWav(const std::filesystem::path& path,
                                         const WAVHeader& header) {
#if defined(_WIN32)
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
                   ? ExclusiveCreateResult::AlreadyExists
                   : ExclusiveCreateResult::Failed;
    }
    DWORD written = 0;
    const bool ok =
        ::WriteFile(file, &header, DWORD(sizeof(header)), &written, nullptr) !=
            FALSE &&
        written == sizeof(header);
    const bool closed = ::CloseHandle(file) != FALSE;
    if (!ok || !closed) {
        ::DeleteFileW(path.c_str());
        return ExclusiveCreateResult::Failed;
    }
    return ExclusiveCreateResult::Created;
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        return errno == EEXIST ? ExclusiveCreateResult::AlreadyExists
                              : ExclusiveCreateResult::Failed;
    }
    const char* bytes = reinterpret_cast<const char*>(&header);
    std::size_t offset = 0;
    bool ok = true;
    while (offset < sizeof(header)) {
        const ssize_t count =
            ::write(descriptor, bytes + offset, sizeof(header) - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += std::size_t(count);
    }
    if (::close(descriptor) != 0) ok = false;
    if (!ok) {
        ::unlink(path.c_str());
        return ExclusiveCreateResult::Failed;
    }
    return ExclusiveCreateResult::Created;
#endif
}

std::atomic<uint64_t> g_recordingPathSequence{1};

} // namespace

AudioRecorder::AudioRecorder() = default;
AudioRecorder::~AudioRecorder() { shutdown(); }

Result AudioRecorder::initialize(SampleRate sampleRate, uint32_t channels) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return Result::ok();

    if (!std::isfinite(sampleRate) || sampleRate < kMinSampleRate || sampleRate > kMaxSampleRate || channels < 1 || channels > 2)
        return Result::fail(EngineError::InvalidArgument);
    m_sampleRate = sampleRate;
    m_peakBucketFrames = std::max<std::uint32_t>(1, std::uint32_t(std::llround(sampleRate / 40.0)));
    m_fileChannels = static_cast<ChannelCount>(channels);
    m_inputRouting.store(channels);
    m_session.sampleRate = sampleRate;
    m_session.channelCount = m_fileChannels;

    // Roughly four seconds of headroom, so a busy disk cannot cost us audio.
    const auto samples = static_cast<size_t>(sampleRate) * m_fileChannels * 4;
    m_ringCapacity = nextPowerOfTwo(std::max<size_t>(samples, 1 << 16));
    m_ring.assign(m_ringCapacity, 0.0f);
    m_ringPositions.resize(m_ringCapacity / m_fileChannels);
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
                                     ChannelCount count, bool enabled) {
    m_inputRouting.store((std::uint64_t(firstChannel) << 32) |
        (enabled ? std::uint32_t(std::clamp<ChannelCount>(count, 1, 2)) : 0u),
        std::memory_order_relaxed);
}

void AudioRecorder::publishPeak() noexcept {
    m_peakHistory[m_peakIndex % kPeakHistoryBuckets].store(
        ((m_peakIndex + 1) << 32) | std::bit_cast<std::uint32_t>(m_peak),
        std::memory_order_relaxed);
}

bool AudioRecorder::readPeakBucket(std::uint64_t index, float& peak) const noexcept {
    const auto value = m_peakHistory[index % kPeakHistoryBuckets].load(std::memory_order_relaxed);
    if ((value >> 32) != index + 1) return false;
    peak = std::bit_cast<float>(std::uint32_t(value));
    return true;
}

std::string AudioRecorder::makeRecordingPath(TrackID trackID,
                                             uint64_t nonce) const {
    namespace fs = std::filesystem;

    fs::path directory = m_recordPath.empty()
        ? fs::temp_directory_path() / "VLTONE Recordings"
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

    char name[128];
    std::snprintf(name, sizeof(name), "Track %u %s-%016llx.wav",
                  static_cast<unsigned>(trackID), stamp,
                  static_cast<unsigned long long>(nonce));
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
        m_session.capturedFrames = 0;
        m_session.writtenFrames = 0;
        m_session.droppedFrames = 0;
        m_session.fileWriteSucceeded = false;
        m_session.sampleRate = m_sampleRate;
        m_session.channelCount = m_fileChannels;
        m_session.filePath.clear();
        // Reserve space for the header; it is rewritten with the real sizes
        // when the recording stops. The exclusive create is also the filename
        // allocator, so simultaneous recorders can never truncate one another.
        const auto header = makeHeader(
            static_cast<uint16_t>(m_fileChannels),
            static_cast<uint32_t>(m_sampleRate), 0);
        const uint64_t clockNonce = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now()
                .time_since_epoch()
                .count());
        constexpr int kMaximumNameAttempts = 256;
        for (int attempt = 0; attempt < kMaximumNameAttempts; ++attempt) {
            const uint64_t sequence =
                g_recordingPathSequence.fetch_add(1, std::memory_order_relaxed);
            const std::string candidate =
                makeRecordingPath(trackID, clockNonce ^ sequence);
            const ExclusiveCreateResult created = createExclusiveWav(
                daw::platform::pathFromUtf8(candidate), header);
            if (created == ExclusiveCreateResult::Created) {
                m_session.filePath = candidate;
                break;
            }
            if (created == ExclusiveCreateResult::Failed) break;
        }
        if (m_session.filePath.empty()) {
            m_session.state = RecordingSession::State::Idle;
            return Result::fail(EngineError::FileWriteError,
                                "Could not create a unique recording file");
        }
    }

    m_writeIndex.store(0, std::memory_order_relaxed);
    m_readIndex.store(0, std::memory_order_relaxed);
    m_droppedFrames.store(0, std::memory_order_relaxed);
    m_recordedFrames.store(0, std::memory_order_relaxed);
    m_writtenFrames.store(0, std::memory_order_relaxed);
    m_writerFailures.store(0, std::memory_order_relaxed);
    m_startSample.store(startSample);
    m_startLatched = false;
    m_initialSkip = 0;
    m_inputXruns.store(0);
    m_interrupted.store(false);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_session.state = RecordingSession::State::Recording;
    }
    m_peak = 0.0f; m_peakIndex = 0; m_peakFrames = 0;
    for (auto& bucket : m_peakHistory) bucket.store(0, std::memory_order_relaxed);
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
        const TimeSamples captured =
            m_recordedFrames.load(std::memory_order_relaxed);
        m_session.recordedSamples = captured;
        m_session.capturedFrames = captured;
        m_session.writtenFrames =
            m_writtenFrames.load(std::memory_order_relaxed);
        m_session.droppedFrames =
            m_droppedFrames.load(std::memory_order_relaxed);
        m_session.fileWriteSucceeded =
            m_writerFailures.load(std::memory_order_relaxed) == 0;
        m_session.startSample = m_startSample.load();
        m_session.inputXruns = m_inputXruns.load();
        m_session.interrupted = m_interrupted.load();
        m_session.state = RecordingSession::State::Stopped;
        completed = m_session;
        m_session.state = RecordingSession::State::Idle;
        completeCallback = m_completeCallback;
    }

    if (completeCallback) {
        completeCallback(completed);
    }
    return completed.fileWriteSucceeded
        ? Result::ok()
        : Result::fail(EngineError::FileWriteError,
                       "Recording file write did not complete");
}

void AudioRecorder::process(const AudioBuffer* input, BufferSize frames,
                            BufferSize offset, bool inputXrun, std::optional<TimeSamples> inputPosition) {
    if (!m_recording.load(std::memory_order_acquire)) return;
    m_processInFlight.fetch_add(1, std::memory_order_acq_rel);
    struct ProcessGuard {
        std::atomic<std::uint32_t>& count;
        ~ProcessGuard() { count.fetch_sub(1, std::memory_order_release); }
    } guard{m_processInFlight};
    if (!m_recording.load(std::memory_order_acquire) || !m_ringCapacity || !frames) return;
    if (!m_startLatched) {
        if (inputPosition) {
            m_startSample.store(std::max<TimeSamples>(0, *inputPosition), std::memory_order_release);
            m_initialSkip = std::max<TimeSamples>(0, -*inputPosition);
        }
        m_startLatched = true;
    }
    const auto skip = BufferSize(std::min<TimeSamples>(frames, m_initialSkip));
    m_initialSkip -= skip; frames -= skip; offset += skip;
    if (!frames) return;
    if (inputXrun) m_inputXruns.fetch_add(1, std::memory_order_relaxed);
    const auto start = m_recordedFrames.load(std::memory_order_relaxed);
    const auto routing = m_inputRouting.load(std::memory_order_relaxed);
    const auto first = ChannelCount(routing >> 32);
    const auto count = ChannelCount(std::uint32_t(routing));
    const bool missing = count && (!input || !input->isValid() ||
        offset > input->numFrames() || frames > input->numFrames() - offset ||
        first >= input->numChannels() || count > input->numChannels() - first);
    const size_t mask = m_ringCapacity - 1;
    const size_t write = m_writeIndex.load(std::memory_order_relaxed);
    const size_t read = m_readIndex.load(std::memory_order_acquire);
    const bool fits = size_t(frames) * m_fileChannels <= mask - ((write - read) & mask);
    if (missing || !fits) m_droppedFrames.fetch_add(frames, std::memory_order_relaxed);
    const float* sources[2]{};
    for (ChannelCount ch = 0; ch < m_fileChannels; ++ch)
        if (count && input && input->isValid() && first + (count >= 2 ? ch : 0) < input->numChannels())
            sources[ch] = input->getChannel(first + (count >= 2 ? ch : 0));
    size_t cursor = write;
    for (BufferSize frame = 0; frame < frames; ++frame) {
        if (fits) m_ringPositions[cursor / m_fileChannels] = start + frame;
        for (ChannelCount ch = 0; ch < m_fileChannels; ++ch) {
            const float* data = sources[ch];
            const float sample = data && offset + frame < input->numFrames() ? data[offset + frame] : 0.0f;
            if (fits) { m_ring[cursor] = sample; cursor = (cursor + 1) & mask; }
            if (fits && std::isfinite(sample)) m_peak = std::max(m_peak, std::abs(sample));
        }
        if (++m_peakFrames == m_peakBucketFrames) {
            publishPeak(); ++m_peakIndex; m_peakFrames = 0; m_peak = 0.0f;
        }
    }
    if (m_peakFrames) publishPeak();
    if (fits) m_writeIndex.store(cursor, std::memory_order_release);
    // Even a lost block occupies time. The writer fills holes using each
    // queued frame's absolute position; later audio never slides left.
    m_recordedFrames.store(start + frames, std::memory_order_release);
}

void AudioRecorder::latchWriterFailure(WriterFailure failure) noexcept {
    m_writerFailures.fetch_or(static_cast<std::uint32_t>(failure),
                              std::memory_order_relaxed);
}

void AudioRecorder::writerLoop() {
    std::ofstream file(daw::platform::pathFromUtf8(m_session.filePath),
                       std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
        latchWriterFailure(WriterOpenFailed);
        return;
    }
    file.seekp(0, std::ios::end);
    if (!file) {
        latchWriterFailure(WriterOpenFailed);
        file.close();
        return;
    }

    const size_t mask = m_ringCapacity - 1;
    const size_t channels =
        std::max<size_t>(static_cast<size_t>(m_fileChannels), 1);
    uint64_t samplesWritten = 0;
    std::vector<float> chunk(4096 * channels);
    bool dataWritable = true;

    const auto writeChunk = [&](size_t count) {
        if (!dataWritable) return;
        file.write(reinterpret_cast<const char*>(chunk.data()),
                   static_cast<std::streamsize>(count * sizeof(float)));
        if (file) {
            samplesWritten += count;
            m_writtenFrames.store(TimeSamples(samplesWritten / channels), std::memory_order_relaxed);
        } else {
            latchWriterFailure(WriterDataWriteFailed);
            dataWritable = false;
        }
    };
    const auto padTo = [&](TimeSamples position) {
        while (dataWritable && TimeSamples(samplesWritten / channels) < position) {
            const size_t count = std::min<uint64_t>(chunk.size(),
                uint64_t(position - TimeSamples(samplesWritten / channels)) * channels);
            std::fill_n(chunk.data(), count, 0.0f);
            writeChunk(count);
        }
    };
    const auto drain = [&] {
        while (true) {
            const size_t write = m_writeIndex.load(std::memory_order_acquire);
            const size_t read = m_readIndex.load(std::memory_order_relaxed);
            const size_t available = ((write - read) & mask) / channels;
            if (!available) break;
            const auto position = m_ringPositions[read / channels];
            padTo(position);
            size_t frames = 1;
            while (frames < std::min(available, chunk.size() / channels) &&
                m_ringPositions[((read + frames * channels) & mask) / channels] == position + TimeSamples(frames)) ++frames;
            const size_t count = frames * channels;
            if (dataWritable) {
                for (size_t i = 0; i < count; ++i) chunk[i] = m_ring[(read + i) & mask];
                writeChunk(count);
            }
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
    padTo(m_recordedFrames.load(std::memory_order_acquire));
    file.flush();
    if (!file) latchWriterFailure(WriterFlushFailed);
    file.close();
    if (file.fail()) latchWriterFailure(WriterFlushFailed);

    if (!finalizeFile(samplesWritten / channels))
        latchWriterFailure(WriterHeaderFinalizeFailed);
}

bool AudioRecorder::finalizeFile(uint64_t framesWritten) {
    std::fstream file(daw::platform::pathFromUtf8(m_session.filePath),
                      std::ios::binary | std::ios::in | std::ios::out);
    if (!file) return false;

    const auto blockAlign = static_cast<uint16_t>(m_fileChannels * 4);
    bool succeeded = true;
    if (framesWritten > maxWavFrames(blockAlign)) {
        DAW_LOG_ERROR("[Recording] take is longer than a WAV header can "
                      "describe (%llu frames); the file says %llu",
                      static_cast<unsigned long long>(framesWritten),
                      static_cast<unsigned long long>(maxWavFrames(blockAlign)));
        succeeded = false;
    }
    const auto header = makeHeader(static_cast<uint16_t>(m_fileChannels),
                                   static_cast<uint32_t>(m_sampleRate),
                                   framesWritten);
    file.seekp(0, std::ios::beg);
    if (!file) succeeded = false;
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (!file) succeeded = false;
    file.flush();
    if (!file) succeeded = false;
    file.close();
    if (file.fail()) succeeded = false;
    return succeeded;
}

void AudioRecorder::setRecordingCompleteCallback(
    RecordingCompleteCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_completeCallback = std::move(cb);
}

RecordingSession AudioRecorder::session() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    RecordingSession copy = m_session;
    copy.startSample = m_startSample.load();
    copy.inputXruns = m_inputXruns.load();
    copy.interrupted = m_interrupted.load();
    if (copy.state == RecordingSession::State::Recording) {
        const TimeSamples captured =
            m_recordedFrames.load(std::memory_order_relaxed);
        copy.recordedSamples = captured;
        copy.capturedFrames = captured;
        copy.writtenFrames = m_writtenFrames.load(std::memory_order_relaxed);
        copy.droppedFrames = m_droppedFrames.load(std::memory_order_relaxed);
        copy.fileWriteSucceeded = false;
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
