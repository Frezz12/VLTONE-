#pragma once

#include "Types.hpp"
#include "AudioBuffer.hpp"
#include "AudioWorkerConfig.hpp"

namespace audio {

struct AudioCallbackContext {
    AudioBuffer* outputBuffer = nullptr;
    const AudioBuffer* inputBuffer = nullptr;
    TimeSamples sampleTime = 0;
    BufferSize numFrames = 0;
    SampleRate sampleRate = 0.0;
    bool isRealtime = true;
    std::int64_t outputTimeNs = 0; // steady-clock time of the first output sample
    bool outputTimeIsDeviceTimestamp = false;
    enum class RenderStatus { NotRendered = -1, Complete = 0, Gated = 1, Failed = 2 };
    RenderStatus renderStatus = RenderStatus::NotRendered;
    std::int64_t inputTimeNs = 0;
    bool inputTimeIsDeviceTimestamp = false;
    std::uint32_t statusFlags = 0;
};

class IAudioCallback {
public:
    virtual ~IAudioCallback() = default;
    /// True only when every sample of every output channel is defined before
    /// onAudioCallback returns. The device keeps its defensive pre-clear for
    /// the default/partial-writer contract.
    virtual bool writesCompleteOutput() const noexcept { return false; }
    // Device-control thread only, with the stream stopped. Never called by the
    // PortAudio render trampoline.
    virtual void configureAudioWorkers(const daw::rt::AudioWorkerConfig&) {}
    virtual void onAudioCallback(AudioCallbackContext& ctx) = 0;
};

class IAudioDeviceNotification {
public:
    virtual ~IAudioDeviceNotification() = default;
    virtual void onDeviceSampleRateChanged(SampleRate newRate) = 0;
    virtual void onDeviceBufferSizeChanged(BufferSize newSize) = 0;
    virtual void onDeviceLost() = 0;
    virtual void onDeviceRestarted() = 0;
};

} // namespace audio
