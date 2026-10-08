#pragma once

#include "AudioRuntimeCoreFields.hpp"
#include "AudioRuntimePluginService.hpp"
#include "AudioRuntimeState.hpp"
#include "AudioMiniModuleCompiler.hpp"
#include "Internal/SamplerVoice.hpp"

namespace daw::audio_value {

// These are field descriptions for the shared bounded value codec. Immutable
// SampleBuffer members use its resource registry; no native address is encoded.
#define DAW_PLUGIN_FIELDS(Type, ...) template<class A> void fields(A& a, Type& v) { a(__VA_ARGS__); }
DAW_PLUGIN_FIELDS(PluginIdentity, v.project, v.instance)
DAW_PLUGIN_FIELDS(AudioPluginControlChange, v.bypassed, v.mix)
DAW_PLUGIN_FIELDS(AudioPluginCapabilities, v.available, v.dualMono, v.sidechain)
DAW_PLUGIN_FIELDS(AudioPluginSlideStatus, v.mode, v.overloaded, v.clipped)
DAW_PLUGIN_FIELDS(plugins::ParameterInfo, v.index, v.id, v.name, v.unit,
    v.minValue, v.maxValue, v.defaultValue, v.isAutomatable, v.isStepped, v.isBypass)
DAW_PLUGIN_FIELDS(PluginParameterReadout, v.id, v.index, v.value, v.available)
DAW_PLUGIN_FIELDS(PluginEditorSize, v.width, v.height, v.resizable)
DAW_PLUGIN_FIELDS(EffectMeterSnapshot, v.available, v.input, v.output, v.reduction,
    v.wet, v.inputHz, v.targetHz, v.correctionCents, v.latencySamples, v.quality, v.qualityPending)

DAW_PLUGIN_FIELDS(AudioPluginStateRequest, v.address, v.includeState, v.packagedSample, v.purpose)
DAW_PLUGIN_FIELDS(AudioPluginStateSnapshot, v.address, v.descriptor, v.exists,
    v.failed, v.isolated, v.supportsState, v.stateCaptured,
    v.documentParametersAuthoritative, v.ownsSample, v.samplePath, v.sample,
    v.state, v.parameters, v.pending)
DAW_PLUGIN_FIELDS(AudioSessionPublication, v.imported, v.plugins)
DAW_PLUGIN_FIELDS(AudioPluginServiceResult, v.changed, v.scanned, v.notices, v.error)

DAW_PLUGIN_FIELDS(plugins::equalizer::AnalyzerConfig, v.enabled, v.pre, v.post,
    v.sidechain, v.frozen, v.speed, v.tiltDbPerOctave)
DAW_PLUGIN_FIELDS(plugins::equalizer::Telemetry, v.pre, v.post, v.sidechain,
    v.dynamicGainDb, v.inputLeft, v.inputRight, v.outputLeft, v.outputRight, v.sidechainPresent)
DAW_PLUGIN_FIELDS(EqualizerSnapshot, v.bands, v.analyzer, v.telemetry, v.comparison, v.preset)
DAW_PLUGIN_FIELDS(EqualizerResponse, v.combined, v.bands)
DAW_PLUGIN_FIELDS(plugins::gravity::Telemetry, v.inputLeft, v.inputRight,
    v.outputLeft, v.outputRight, v.fieldEnergy, v.orbitPhase, v.duckGain,
    v.transientPulse, v.grainSerial, v.activeGrains, v.frozen)
DAW_PLUGIN_FIELDS(GravitySnapshot, v.telemetry, v.frozen, v.lastPreset, v.preset)

// The immutable wrapper is an owned value. Its nested audio is mapped once
// per resource ID, shared by sampler, slicer and state replies alike.
DAW_PLUGIN_FIELDS(plugins::sampler::SampleData, v.audio, v.baseFrames, v.path, v.name)
DAW_PLUGIN_FIELDS(SamplerSnapshot, v.available, v.precomputePending, v.path, v.name, v.sample, v.hasSource)
DAW_PLUGIN_FIELDS(plugins::slicer::Slice, v.start, v.end, v.key, v.transpose,
    v.gain, v.pan, v.cutoff, v.resonance, v.filter, v.chokeGroup, v.flags,
    v.id, v.fineTune, v.normalization, v.fadeInMs, v.fadeOutMs, v.crossfadeMs,
    v.loopMode, v.locked, v.useGlobalEnvelope, v.attack, v.decay, v.sustain,
    v.release, v.effect, v.effectX, v.effectY, v.effectMix)
DAW_PLUGIN_FIELDS(plugins::slicer::ControlState, v.path, v.audio, v.table, v.analysis, v.parameters)
DAW_PLUGIN_FIELDS(SlicerSnapshot, v.identity, v.state, v.name, v.sourceRevision, v.activeKeys)

DAW_PLUGIN_FIELDS(AudioMiniModuleCompileRequest::Target, v.address, v.spec)
DAW_PLUGIN_FIELDS(AudioMiniModuleCompileRequest, v.info, v.targets)
DAW_PLUGIN_FIELDS(AudioMiniModuleCompileStatus::Target, v.address, v.latency)
#undef DAW_PLUGIN_FIELDS

template<class A> void fields(A& a, PluginEditorSnapshot& v) {
    a(v.identity, v.name, v.uid, v.format, v.remote, v.hasEditor, v.open, v.openFailed, v.processId, v.pending);
    coreEnum(v.format, plugins::Format::Vst);
}
template<class A> void fields(A& a, AudioPluginRuntimeStatus& v) {
    a(v.state, v.detail);
    coreEnum(v.state, AudioPluginRuntimeState::Missing);
}
template<class A> void fields(A& a, AudioPluginNotice& v) {
    a(v.kind, v.address, v.chain, v.clipId, v.parameterId, v.value, v.touch);
    coreEnum(v.kind, AudioPluginNotice::Kind::StateChanged);
    coreEnum(v.chain, AudioPluginChainSpec::Kind::ClipFx);
}
template<class A> void fields(A& a, plugins::equalizer::BandState& v) {
    a(v.enabled, v.type, v.frequency, v.gainDb, v.q, v.slope, v.placement,
        v.dynamicEnabled, v.dynamicRangeDb, v.dynamicAuto, v.thresholdDb,
        v.attackMs, v.releaseMs, v.externalSidechain, v.detectorMode, v.detectorLow, v.detectorHigh);
    coreEnum(v.type, plugins::equalizer::FilterType::AllPass);
    coreEnum(v.slope, plugins::equalizer::Slope::Db96);
    coreEnum(v.placement, plugins::equalizer::Placement::Side);
    coreEnum(v.detectorMode, plugins::equalizer::DetectorMode::Free);
}
template<class A> void fields(A& a, plugins::slicer::AnalysisSettings& v) {
    a(v.mode, v.targetCount, v.sensitivity, v.seed, v.preAttackMs, v.rootNote,
        v.scale, v.descending, v.minimumMs, v.zeroCrossing, v.randomSpread,
        v.sourceBpm, v.gridBeats, v.rangeStart, v.rangeEnd);
    coreEnum(v.mode, plugins::slicer::SliceMode::Manual);
}
template<class A> void fields(A& a, plugins::slicer::SliceTable& v) {
    a(v.count, v.frames, v.nextId, v.chromaticFallback);
    if (v.count > plugins::slicer::kMaxSlices) invalid("slicer table exceeds slice limit");
    // Unused array capacity and derived key indices are not state. Rebuild the
    // lookup from bounded owned slices, using the same rules as native edits.
    for (std::uint32_t i = 0; i < v.count; ++i) a(v.slices[i]);
    if constexpr (A::reading) v.rebuild();
}
template<class A> void fields(A& a, AudioMiniModuleCompileStatus& v) {
    a(v.state, v.error, v.targets);
    coreEnum(v.state, AudioMiniModuleCompileStatus::State::Failed);
}

} // namespace daw::audio_value
