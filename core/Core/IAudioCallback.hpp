#pragma once

#include "Types.hpp"
#include "AudioBuffer.hpp"

namespace audio {

struct AudioCallbackContext {
    AudioBuffer* outputBuffer = nullptr;
    const AudioBuffer* inputBuffer = nullptr;
    TimeSamples sampleTime = 0;
    BufferSize numFrames = 0;
    SampleRate sampleRate = 0.0;
    bool isRealtime = true;
};

class IAudioCallback {
public:
    virtual ~IAudioCallback() = default;
    /// True only when every sample of every output channel is defined before
    /// onAudioCallback returns. The device keeps its defensive pre-clear for
    /// the default/partial-writer contract.
    virtual bool writesCompleteOutput() const noexcept { return false; }
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
