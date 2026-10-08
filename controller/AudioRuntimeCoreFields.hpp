#pragma once

#include "AudioRuntimeRecording.hpp"
#include "AudioValueCodec.hpp"
#include "DSP/LoudnessMeter.hpp"
#include "Graph/Node.hpp"

namespace daw {

/// Result has a private default constructor. Keep its owned wire value public
/// so a complete reply can be decoded before any caller-visible state changes.
struct AudioResultValue {
    audio::EngineError code = audio::EngineError::None;
    std::string message;
    AudioResultValue() = default;
    AudioResultValue(const audio::Result& value) : code(value.error()), message(value.message()) {}
    audio::Result result() const { return audio::Result::fail(code, message); }
};

} // namespace daw

namespace daw::audio_value {

template<class Enum> void coreEnum(Enum value, Enum last) {
    if (std::uint32_t(value) > std::uint32_t(last)) invalid("unknown audio core enum");
}

#define DAW_CORE_FIELDS(Type, ...) template<class A> void fields(A& a, Type& v) { a(__VA_ARGS__); }
DAW_CORE_FIELDS(AudioMeterSnapshot, v.left, v.right, v.hold)
DAW_CORE_FIELDS(AudioRuntimeDiagnostics, v.load, v.latency, v.lastRenderError,
    v.failedBlocks, v.gatedBlocks, v.droppedProfileEvents, v.workers,
    v.realtimeWorkers, v.workgroupWorkers)
DAW_CORE_FIELDS(AudioPreviewSnapshot, v.playing, v.positionSeconds)
DAW_CORE_FIELDS(rt::BlockTiming, v.elapsedNs, v.budgetNs, v.frames, v.flags)
DAW_CORE_FIELDS(rt::TimingCounters, v.blocks, v.overruns, v.maximumNs, v.dropped)
DAW_CORE_FIELDS(AudioTimingSnapshot, v.counters, v.events)
DAW_CORE_FIELDS(AudioCaptureStarted, v.id, v.path, v.peakBucketFrames)
DAW_CORE_FIELDS(AudioCaptureStatus, v.available, v.recording, v.recordedFrames,
    v.startSample, v.peakBucketFrames, v.sampleRate)
DAW_CORE_FIELDS(AudioCapturePeaks, v.status, v.firstBucket, v.values)
DAW_CORE_FIELDS(audio::DeviceInfo, v.uid, v.name, v.manufacturer, v.hostApi,
    v.isAsio, v.hasControlPanel, v.inputChannels, v.outputChannels,
    v.inputChannelNames, v.outputChannelNames, v.sampleRates, v.bufferSizes,
    v.preferredBufferSize, v.isDefaultInput, v.isDefaultOutput, v.isAlive)
#undef DAW_CORE_FIELDS

template<class A> void fields(A& a, AudioResultValue& v) {
    a(v.code, v.message); coreEnum(v.code, audio::EngineError::FileWriteError);
}
template<class A> void fields(A& a, rt::ProfileEvent& v) {
    a(v.generation, v.elapsedNs, v.position, v.node, v.worker, v.kind);
    coreEnum(v.kind, rt::ProfileEvent::Kind::Wait);
}
template<class A> void fields(A& a, AudioTransportSnapshot& v) {
    a(v.state, v.position, v.duration, v.loopStart, v.loopEnd, v.positionSeconds,
        v.presentationSeconds, v.tempo, v.playing, v.recording, v.loopEnabled);
    coreEnum(v.state, engine::TransportState::Recording);
}
template<class A> void fields(A& a, AudioDeviceSnapshot& v) {
    a(v.running, v.hasStream, v.inputUsesDeviceTime, v.state, v.sampleRate,
        v.bufferSize, v.callbacks, v.renderCalls, v.lastCallbackNs, v.lastRenderStatus, v.xruns);
    coreEnum(v.state, audio::AudioDeviceState::Failed);
}
template<class A> void fields(A& a, audio::AudioDeviceConfig& v) {
    a(v.outputDeviceUid, v.inputDeviceUid, v.inputEnabled, v.sampleRate,
        v.bufferSize, v.inputChannelSelectors, v.outputChannelSelectors);
    if (v.sampleRate < 1000 || v.sampleRate > 768000 ||
        !v.bufferSize || v.bufferSize > engine::kMaxBlockSize)
        invalid("invalid audio device configuration");
    for (const auto* selectors : {&v.inputChannelSelectors, &v.outputChannelSelectors})
        for (const auto channel : *selectors)
            if (channel < 0) invalid("invalid audio channel selector");
}
template<class A> void fields(A& a, engine::PrepareInfo& v) {
    a(v.sampleRate, v.maxBlockSize, v.channels, v.offline);
    if (v.sampleRate < 1000 || v.sampleRate > 768000 || !v.maxBlockSize ||
        v.maxBlockSize > engine::kMaxBlockSize || !v.channels || v.channels > engine::kMaxChannels)
        invalid("invalid audio preparation");
}
template<class A> void fields(A& a, AudioCaptureSpec& v) {
    a(v.directory, v.inputChannel, v.channelCount, v.inputEnabled, v.startSample);
    if (!v.channelCount || v.channelCount > 2 || v.inputChannel >= engine::kMaxChannels)
        invalid("invalid audio capture configuration");
}
template<class A> void fields(A& a, audio::RecordingSession& v) {
    a(v.state, v.filePath, v.trackID, v.startSample, v.recordedSamples,
        v.capturedFrames, v.writtenFrames, v.droppedFrames, v.inputXruns,
        v.interrupted, v.fileWriteSucceeded, v.channelCount, v.sampleRate);
    coreEnum(v.state, audio::RecordingSession::State::Stopped);
}

/// The meter distinguishes unavailable (NaN), measured silence (-infinity)
/// and finite LUFS. Encode those states explicitly: silence must not reject
/// the whole UI readout batch and disconnect an otherwise healthy engine.
template<class A> void fields(A& a, engine::LoudnessLevels& v) {
    const auto level = [&](float& value) {
        std::uint8_t kind = 0; // unavailable, finite, silence
        if constexpr (!A::reading) {
            if (std::isfinite(value)) kind = 1;
            else if (value == -std::numeric_limits<float>::infinity()) kind = 2;
            else if (!std::isnan(value)) invalid("invalid positive infinite loudness");
        }
        a(kind);
        switch (kind) {
        case 0: if constexpr (A::reading) value = std::numeric_limits<float>::quiet_NaN(); break;
        case 1: a(value); break;
        case 2: if constexpr (A::reading) value = -std::numeric_limits<float>::infinity(); break;
        default: invalid("unknown loudness reading state");
        }
    };
    level(v.momentary); level(v.shortTerm); level(v.integrated);
}

// RealtimeEngine::MasterSpectrum is already a fixed std::array<float, 12>.
// Shared PCM fields use AudioValueCodec's existing resource-ID specialization.

} // namespace daw::audio_value
