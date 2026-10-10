#include "AudioRuntime.hpp"
#include "SampleLoader.hpp"
#include "Internal/SampleDecoder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace daw {

// ── Device bridge ──────────────────────────────────────────────────────────

/// Turns one PortAudio callback into one engine block. This is the only place
/// where the platform layer and the engine meet; it does no processing of its
/// own beyond handing the recorder the raw input.
class AudioRuntime::DeviceCallback final : public audio::IAudioCallback {
public:
    explicit DeviceCallback(AudioRuntime& runtime)
        : m_runtime(runtime), m_engine(runtime.engine), m_recorders(runtime.activeRecorders) {}

    bool writesCompleteOutput() const noexcept override { return true; }
    void configureAudioWorkers(const rt::AudioWorkerConfig& config) override {
        m_engine.configureAudioWorkers(config);
        if (m_runtime.auditionRuntime) m_runtime.auditionRuntime->configureAudioWorkers(config);
        m_workerConfiguration = config;
    }
    const rt::AudioWorkerConfig& workerConfiguration() const noexcept { return m_workerConfiguration; }

    void onAudioCallback(audio::AudioCallbackContext& ctx) override {
        if (!ctx.outputBuffer) return;
        const engine::FrameCount frames =
            std::min<engine::FrameCount>(ctx.numFrames, ctx.outputBuffer->numFrames());
        if (frames == 0) return;
        if (m_runtime.safetyStopped.load(std::memory_order_acquire)) {
            for (std::size_t ch = 0; ch < ctx.outputBuffer->numChannels(); ++ch)
                std::fill_n(ctx.outputBuffer->getChannel(ch), frames, 0.0f);
            ctx.renderStatus = audio::AudioCallbackContext::RenderStatus::Failed;
            const auto recorders = m_recorders.read();
            if (recorders) for (const auto& recorder : *recorders)
                if (recorder && recorder->isRecording()) recorder->markInterrupted();
            return;
        }

        // The device buffer is already planar, so the engine renders straight
        // into it — no copy, no interleave.
        const auto deviceChannels = ctx.outputBuffer->numChannels();
        const engine::ChannelCount channels = engine::ChannelCount(
            std::min<std::size_t>(deviceChannels, engine::kMaxChannels));
        for (engine::ChannelCount ch = 0; ch < channels; ++ch) {
            m_outputPointers[ch] = ctx.outputBuffer->getChannel(ch);
        }
        engine::AudioBlock output(m_outputPointers.data(), channels, frames);

        // The graph has a deliberate fixed channel ceiling. A wider device is
        // still safe: render the supported prefix and define every remaining
        // output channel as silence instead of overrunning the pointer array.
        for (std::size_t ch = channels; ch < deviceChannels; ++ch) {
            std::fill_n(ctx.outputBuffer->getChannel(ch), frames, 0.0f);
        }

        engine::ChannelCount inputChannels = 0;
        const float* const* inputChannelData = nullptr;
        if (ctx.inputBuffer) {
            inputChannels = engine::ChannelCount(std::min<std::size_t>(
                ctx.inputBuffer->numChannels(), engine::kMaxChannels));
            for (engine::ChannelCount ch = 0; ch < inputChannels; ++ch) {
                m_inputPointers[ch] = ctx.inputBuffer->getChannel(ch);
            }
            inputChannelData = m_inputPointers.data();
        }

        m_engine.transport().setPresentationTiming(ctx.outputTimeNs,
            ctx.outputTimeIsDeviceTimestamp ? engine::PresentationClockSource::DeviceTimestamp :
            ctx.outputTimeNs > 0 ? engine::PresentationClockSource::DeviceLatency :
                                  engine::PresentationClockSource::RenderEstimate);
        auto recorders = m_recorders.read(); // one capture snapshot for this entire block
        m_engine.renderBlock(output, inputChannelData, inputChannels, frames);
        using BlockResult = engine::RealtimeEngine::BlockResult;
        ctx.renderStatus = m_engine.lastBlockResult() == BlockResult::Complete
            ? audio::AudioCallbackContext::RenderStatus::Complete
            : m_engine.lastBlockResult() == BlockResult::Gated
                ? audio::AudioCallbackContext::RenderStatus::Gated
                : audio::AudioCallbackContext::RenderStatus::Failed;

        // Recording taps the hardware input, not the mix: capturing the master
        // would print everything already on the timeline into the new take.
        // Each armed track has its own recorder, and each picks its own input
        // channels out of this same buffer.
        if (recorders && !recorders->empty()) {
            const auto result = m_engine.lastBlockResult();
            // The graph's latency delays the accompaniment as well as the
            // device's DAC queue. ADC timestamps identify when this input was
            // heard against that accompaniment; subtract each delay once.
            const double ioDelay = ctx.outputTimeNs > 0 && ctx.inputTimeNs > 0
                ? double(ctx.outputTimeNs - ctx.inputTimeNs) * ctx.sampleRate / 1e9 : 0.;
            const auto inputPosition = m_engine.lastBlockPosition() -
                engine::SamplePos(std::llround(ioDelay)) - m_engine.lastBlockLatency();
            for (const auto& recorder : *recorders) {
                if (!recorder || !recorder->isRecording()) continue;
                if (result != BlockResult::Complete) {
                    recorder->markInterrupted();
                    if (result == BlockResult::Gated) continue; // transport did not advance
                }
                recorder->process(ctx.inputBuffer, frames, 0, (ctx.statusFlags & 3u) != 0, inputPosition);
            }
        }
    }

private:
    AudioRuntime& m_runtime;
    engine::RealtimeEngine& m_engine;
    /// Platform workgroup ownership is retained only on the control thread.
    rt::AudioWorkerConfig m_workerConfiguration;
    /// Aliases the runtime's published list, so a capture starting or ending
    /// is picked up on the next block without touching this object.
    engine::RealtimeSnapshot<RecorderList>& m_recorders;
    std::array<float*, engine::kMaxChannels> m_outputPointers{};
    std::array<const float*, engine::kMaxChannels> m_inputPointers{};
};


AudioRuntime::AudioRuntime()
    : devices(std::make_unique<audio::AudioDeviceManager>()),
      callback(std::make_unique<DeviceCallback>(*this)) {
    static std::once_flag decoder;
    std::call_once(decoder, [] {
        plugins::sampler::setSampleDecoder([](const std::string& path) {
            std::shared_ptr<const engine::SampleBuffer> sample;
            return loadSampleBuffer(path, sample) ? sample : nullptr;
        });
    });
}

AudioRuntime::~AudioRuntime() { closeDevice(); stopAudition(); }

audio::Result AudioRuntime::prepare(double rate, std::uint32_t frames, bool offline) {
    if (!std::isfinite(rate) || rate < 1000 || rate > 768000 ||
        frames == 0 || frames > engine::kMaxBlockSize)
        return audio::Result::fail(audio::EngineError::InvalidArgument, "Invalid audio runtime configuration.");
    if (const auto result = engine.prepare(rate, frames, 2, offline); !result)
        return audio::Result::fail(audio::EngineError::InvalidArgument,
            engine.offlineError().empty() ? std::string(engine::describe(result.error()))
                                          : engine.offlineError());
    sampleRate = rate;
    bufferSize = frames;
    prepared = true;
    return audio::Result::ok();
}

audio::Result AudioRuntime::startDevice() {
    // Replacing a session for a new device format already starts its stream.
    // Do not start that PortAudio stream a second time at controller commit.
    if (devices->isRunning()) {
        deviceOpen = liveDeviceAllowed = true;
        return audio::Result::ok();
    }
    deviceOpen = false;
    if (auto attached = devices->setAudioCallback(callback.get()); !attached) return attached;
    auto started = devices->start();
    deviceOpen = bool(started) && devices->isRunning();
    liveDeviceAllowed = deviceOpen;
    return started;
}

void AudioRuntime::closeDevice() {
    liveDeviceAllowed = false;
    if (devices->isInitialized()) {
        devices->setAudioCallback(nullptr);
        devices->stop();
        devices->shutdown();
    }
    deviceOpen = false;
}

void AudioRuntime::processDeviceBlock(audio::AudioCallbackContext& context) {
    callback->onAudioCallback(context);
}

void AudioRuntime::configureAudioWorkers(const rt::AudioWorkerConfig& config) {
    callback->configureAudioWorkers(config);
}

void AudioRuntime::inheritAudioWorkers(AudioRuntime& secondary) const {
    secondary.configureAudioWorkers(callback->workerConfiguration());
}

audio::Result AudioRuntime::takeDeviceFrom(AudioRuntime& previous) {
    if (!previous.devices->isInitialized()) return audio::Result::ok();
    const auto config = previous.deviceConfiguration();
    if (!prepared || config.sampleRate != sampleRate || config.bufferSize > bufferSize)
        return audio::Result::fail(audio::EngineError::SampleRateMismatch,
            "Device format differs from the prepared audio session.");

    const bool running = previous.devices->isRunning();
    const bool allowed = previous.liveDeviceAllowed;
    // setAudioCallback drains the old callback before publishing its successor.
    if (const auto attached = previous.devices->setAudioCallback(callback.get()); !attached)
        return attached;
    if (running) {
        if (const auto started = previous.devices->start(); !started) {
            auto restored = previous.devices->setAudioCallback(previous.callback.get());
            if (restored) restored = previous.devices->start();
            previous.deviceOpen = previous.devices->isRunning();
            if (!restored) {
                previous.stopForFailedRollback();
                return audio::Result::fail(audio::EngineError::DeviceError,
                    started.message() + "; previous audio callback could not be restored: " + restored.message());
            }
            return started;
        }
    }
    devices.swap(previous.devices);
    deviceOpen = running;
    liveDeviceAllowed = allowed;
    previous.deviceOpen = previous.liveDeviceAllowed = false;
    return audio::Result::ok();
}

} // namespace daw
