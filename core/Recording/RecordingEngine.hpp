#pragma once

#include "Core/Types.hpp"
#include "Core/Result.hpp"
#include "Core/AudioBuffer.hpp"
#include <memory>
#include <string>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace audio {

struct RecordingSession {
    enum class State { Idle, Preparing, Recording, Stopped };
    State state = State::Idle;
    std::string filePath;
    TrackID trackID = 0;
    TimeSamples startSample = 0;
    TimeSamples recordedSamples = 0;
    uint32_t channelCount = 2;
    SampleRate sampleRate = 44100;

    bool isRecording() const { return state == State::Recording; }
};

/// Captures the hardware input to a WAV file.
///
/// The audio thread only pushes interleaved samples into a preallocated ring
/// buffer; a writer thread drains it to disk. File I/O from the render callback
/// would stall the audio device.
class AudioRecorder {
public:
    AudioRecorder();
    ~AudioRecorder();

    AudioRecorder(const AudioRecorder&) = delete;
    AudioRecorder& operator=(const AudioRecorder&) = delete;

    Result initialize(SampleRate sampleRate, uint32_t channels);
    void shutdown();

    void setRecordPath(const std::string& path) { m_recordPath = path; }
    const std::string& recordPath() const { return m_recordPath; }

    void setFileFormat(const std::string& format) { m_fileFormat = format; }
    const std::string& fileFormat() const { return m_fileFormat; }

    /// Hardware input channels to capture. `count` of 1 records mono.
    void setInputChannels(ChannelCount firstChannel, ChannelCount count);

    Result startRecording(TrackID trackID, TimeSamples startSample);
    Result stopRecording();
    bool isRecording() const { return m_recording.load(std::memory_order_acquire); }

    /// Realtime-safe: copies `numFrames` of the selected input channels into
    /// the ring buffer. Never allocates, never blocks.
    void process(const AudioBuffer& input, BufferSize numFrames);

    RecordingSession session() const;

    /// Frames captured so far, read without taking the session lock. This is
    /// the recording's own clock — it counts what has actually been written —
    /// so anything drawn against it (the growing take in the arrangement) stays
    /// locked to the file that take will become instead of drifting away from
    /// it on a jittery UI tick.
    TimeSamples recordedFrames() const {
        return m_recordedFrames.load(std::memory_order_relaxed);
    }

    /// Frames dropped because the writer could not keep up.
    uint64_t droppedFrames() const {
        return m_droppedFrames.load(std::memory_order_relaxed);
    }

    using RecordingCompleteCallback = std::function<void(const RecordingSession&)>;
    void setRecordingCompleteCallback(RecordingCompleteCallback cb);

    Result writeWAVFile(const std::string& path, const AudioBuffer& buffer,
                        SampleRate rate);

private:
    void writerLoop();
    void finalizeFile(uint64_t framesWritten);
    std::string makeRecordingPath(TrackID trackID) const;

    RecordingSession m_session;
    std::string m_recordPath;
    std::string m_fileFormat = "wav";
    mutable std::mutex m_mutex;
    RecordingCompleteCallback m_completeCallback;
    bool m_initialized = false;

    // ── Ring buffer shared with the audio thread ──
    // Single producer (audio thread), single consumer (writer thread).
    std::vector<float> m_ring;
    size_t m_ringCapacity = 0;              // in samples, power of two
    std::atomic<size_t> m_writeIndex{0};
    std::atomic<size_t> m_readIndex{0};
    std::atomic<bool> m_recording{false};
    std::atomic<uint64_t> m_droppedFrames{0};
    std::atomic<TimeSamples> m_recordedFrames{0};
    std::atomic<std::uint32_t> m_processInFlight{0};

    ChannelCount m_inputFirstChannel = 0;
    ChannelCount m_inputChannelCount = 2;
    ChannelCount m_fileChannels = 2;
    SampleRate m_sampleRate = kDefaultSampleRate;

    std::thread m_writerThread;
    std::atomic<bool> m_writerRunning{false};
    std::mutex m_writerMutex;
    std::condition_variable m_writerSignal;
};

} // namespace audio
