#include "AudioRuntimePluginFields.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace {
using namespace daw;
namespace wire = daw::audio_value;
int failures = 0;
void check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    failures += !condition;
}
template<class F> bool rejects(F&& action) {
    try { action(); return false; } catch (const std::exception&) { return true; }
}
template<class T> T copy(const T& value) {
    auto [decoded] = wire::decode<T>(wire::encode(value));
    return decoded;
}
struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("daw-plugin-values-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Directory() { std::filesystem::create_directory(path); }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

void metadataAndReadouts() {
    plugins::ParameterInfo parameter{19, "stable.gain", "Gain", "dB", -60, 12, -3, false, true, true};
    const auto metadata = copy(parameter);
    check(metadata.index == 19 && metadata.id == parameter.id && metadata.name == parameter.name &&
        metadata.unit == "dB" && metadata.minValue == -60 && metadata.maxValue == 12 &&
        metadata.defaultValue == -3 && !metadata.isAutomatable && metadata.isStepped && metadata.isBypass,
        "parameter metadata preserves stable IDs, native indexes, range and flags");
    PluginParameterReadout value{"stable.gain", 19, -7.5, true};
    const auto values = copy(std::vector{value});
    check(values.size() == 1 && values[0].index == 19 && values[0].value == -7.5 && values[0].available,
        "batch parameter replies carry owned lookup hints and values");

    EqualizerSnapshot eq;
    eq.bands[23].enabled = true; eq.bands[23].type = plugins::equalizer::FilterType::AllPass;
    eq.bands[23].slope = plugins::equalizer::Slope::Db96;
    eq.bands[23].placement = plugins::equalizer::Placement::Side;
    eq.bands[23].dynamicEnabled = true; eq.bands[23].dynamicRangeDb = -9;
    eq.bands[23].detectorMode = plugins::equalizer::DetectorMode::Free;
    eq.bands[23].detectorLow = 174; eq.bands[23].detectorHigh = 13100;
    eq.analyzer = {true, false, true, true, true, 2, 4.5};
    eq.telemetry.pre[127] = .25f; eq.telemetry.post[126] = .125f;
    eq.telemetry.sidechain[125] = .75f; eq.telemetry.dynamicGainDb[23] = -4;
    eq.telemetry.sidechainPresent = true; eq.comparison = 'B'; eq.preset = {"user", "Test EQ"};
    const auto decoded = copy(eq);
    check(decoded.bands[23].type == eq.bands[23].type && decoded.bands[23].slope == eq.bands[23].slope &&
        decoded.bands[23].placement == eq.bands[23].placement && decoded.bands[23].dynamicRangeDb == -9 &&
        decoded.bands[23].detectorLow == 174 && decoded.bands[23].detectorHigh == 13100 &&
        decoded.analyzer.tiltDbPerOctave == 4.5 && decoded.analyzer.frozen && !decoded.analyzer.pre &&
        decoded.telemetry.pre[127] == .25f && decoded.telemetry.post[126] == .125f &&
        decoded.telemetry.sidechain[125] == .75f && decoded.telemetry.dynamicGainDb[23] == -4 &&
        decoded.telemetry.sidechainPresent && decoded.comparison == 'B' && decoded.preset == eq.preset,
        "EQ band configuration, analyzer spectra and comparison state survive the wire");
    EqualizerResponse response; response.combined[255] = -13; response.bands[23][239] = .7f;
    const auto curve = copy(response);
    check(curve.combined[255] == -13 && curve.bands[23][239] == .7f,
        "EQ response arrays retain their last band and frequency point");
    GravitySnapshot gravity;
    gravity.telemetry.grainSerial = 9007199254741009ull; gravity.telemetry.activeGrains = 29;
    gravity.telemetry.duckGain = .33f; gravity.telemetry.transientPulse = .7f;
    gravity.telemetry.frozen = true; gravity.frozen = true; gravity.lastPreset = 4;
    gravity.preset = {"user", "Orbit"};
    const auto gravityValue = copy(gravity);
    check(gravityValue.telemetry.grainSerial == gravity.telemetry.grainSerial &&
        gravityValue.telemetry.activeGrains == 29 && gravityValue.telemetry.duckGain == .33f &&
        gravityValue.telemetry.transientPulse == .7f && gravityValue.frozen && gravityValue.lastPreset == 4 &&
        gravityValue.preset == gravity.preset, "Gravity telemetry retains exact 64-bit identity and controls");

    PluginEditorSnapshot editor{{9, 31}, "Test", "fixture", plugins::Format::Vst3, true, true, true, false, 123456, true};
    const auto ui = copy(editor);
    check(ui.identity == editor.identity && ui.format == plugins::Format::Vst3 && ui.remote && ui.open &&
        ui.processId == editor.processId && ui.pending &&
        !copy(std::optional<PluginEditorSnapshot>{}).has_value(),
        "editor replies carry numeric identity and explicit absence without host pointers");
}

void resourcesAndState() {
    Directory directory;
    ProcessAudioResources resources(directory.path);
    auto source = std::make_shared<engine::SampleBuffer>(2, 64, 48000);
    for (unsigned i = 0; i < 64; ++i) {
        source->writableChannel(0)[i] = float(i) / 128;
        source->writableChannel(1)[i] = -float(i) / 128;
    }
    auto data = std::make_shared<plugins::sampler::SampleData>();
    data->audio = source; data->baseFrames = 48; data->path = "source.wav"; data->name = "Source";
    SamplerSnapshot sampler{true, true, data->path, data->name, data, true};
    auto slices = std::make_shared<plugins::slicer::SliceTable>();
    slices->count = 2; slices->frames = 64; slices->nextId = 100; slices->chromaticFallback = true;
    auto& slice = slices->slices[0];
    slice.start = 0; slice.end = 31; slice.key = 53; slice.transpose = -7;
    slice.gain = .5f; slice.pan = -.25f; slice.cutoff = .8f; slice.resonance = .1f;
    slice.filter = 2; slice.chokeGroup = 4; slice.flags = plugins::slicer::kSliceReverse;
    slice.id = 10; slice.fineTune = 13; slice.normalization = 1.4f;
    slice.fadeInMs = 3; slice.fadeOutMs = 11; slice.crossfadeMs = 6; slice.loopMode = 2;
    slice.locked = true; slice.useGlobalEnvelope = false; slice.attack = .01f; slice.decay = .2f;
    slice.sustain = .3f; slice.release = .4f; slice.effect = 3;
    slice.effectX = .1f; slice.effectY = .9f; slice.effectMix = .7f;
    slices->slices[1] = slice; slices->slices[1].start = 31; slices->slices[1].end = 64;
    slices->slices[1].key = 57; slices->slices[1].id = 11;
    slices->rebuild();
    SlicerSnapshot slicer;
    slicer.identity = {7, 29}; slicer.state.path = data->path; slicer.state.audio = source;
    slicer.state.table = slices; slicer.state.analysis.mode = plugins::slicer::SliceMode::Manual;
    slicer.state.analysis.seed = 9007199254741007ull; slicer.state.analysis.rangeStart = 7;
    slicer.state.analysis.rangeEnd = 61; slicer.state.parameters[19] = .6;
    slicer.sourceRevision = 103; slicer.activeKeys.set(0); slicer.activeKeys.set(127);
    AudioPluginStateSnapshot state;
    state.address = {"track", "slot", true, 123}; state.exists = state.isolated = state.supportsState = true;
    state.stateCaptured = state.ownsSample = true; state.samplePath = data->path; state.sample = source;
    state.state = {0, 1, 255, 10}; state.parameters = {{"gain", -3, false}};
    state.pending = {{"gain", -9, true}};
    AudioPluginStateRestore restore;
    restore.state = state.state; restore.stateFile = "native.bin"; restore.contentDirectory = "Content";
    restore.sourcePath = data->path; restore.source = source;
    restore.tolerateErrors = restore.applyAllParameters = restore.clearPending = true;
    const auto bytes = wire::encodeResources(resources, sampler, slicer, state, restore);
    ProcessAudioResources::Cache cache;
    auto [gotSampler, gotSlicer, gotState, gotRestore] =
        wire::decodeResources<SamplerSnapshot, SlicerSnapshot, AudioPluginStateSnapshot, AudioPluginStateRestore>(
            bytes, directory.path, &cache);
    check(resources.records().size() == 1 && gotSampler.sample != data && gotSampler.sample->audio != source &&
        gotSampler.sample->audio == gotSlicer.state.audio && gotState.sample == gotSlicer.state.audio &&
        gotRestore.source == gotState.sample && gotState.sample->readSample(1, 63) == -63.f / 128,
        "sampler, slicer and opaque-state DTOs share one mapped PCM resource without pointer transfer");
    check(gotSampler.sample->baseFrames == 48 && gotSampler.precomputePending && gotSampler.hasSource &&
        gotSlicer.state.table->slices[0] == slice && gotSlicer.state.table->slices[1] == slices->slices[1] &&
        gotSlicer.state.table->keyToIndex[53] == 0 && gotSlicer.state.table->keyToIndex[57] == 1 &&
        gotSlicer.state.table->keyToIndex[48] == -1 && gotSlicer.state.table->nextId == 100 &&
        gotSlicer.state.analysis.seed == slicer.state.analysis.seed && gotSlicer.state.parameters[19] == .6 &&
        gotSlicer.activeKeys == slicer.activeKeys && gotSlicer.sourceRevision == 103,
        "slicer slice DSP settings, key lookup, analysis and activity survive decoding");
    check(gotState.address.right && gotState.address.instance == 123 && gotState.state == state.state &&
        gotState.parameters[0].value == -3 && gotState.pending[0].value == -9 &&
        gotState.pending[0].restoreAfterState && gotRestore.stateFile == restore.stateFile &&
        gotRestore.contentDirectory == restore.contentDirectory && gotRestore.tolerateErrors &&
        gotRestore.applyAllParameters && gotRestore.clearPending,
        "native state, pending overrides and explicit right-side address remain independent");
    auto [again] = wire::decodeResources<SamplerSnapshot>(wire::encodeResources(resources, sampler), directory.path, &cache);
    check(again.sample->audio == gotState.sample, "successive replies reuse the acknowledged immutable PCM mapping");
}

void servicesAndValidation() {
    AudioSessionPublication publication;
    auto& imported = publication.imported.emplace_back();
    imported.address = {"track", "restored", false, 42};
    imported.exists = true; imported.parameters = {{"gain", -6, true}};
    publication.plugins.push_back(imported.address);
    const auto acknowledged = copy(publication);
    check(acknowledged.imported.size() == 1 && !acknowledged.imported.front().sample &&
        acknowledged.imported.front().parameters == imported.parameters &&
        acknowledged.plugins.front().instance == 42,
        "publication carries canonical parameters and identities without a PCM registry");
    AudioPluginServiceResult service;
    service.changed = service.scanned = true; service.error = "Native reload failed";
    service.notices.push_back({AudioPluginNotice::Kind::GestureBegin, {"track", "slot", true, 91},
        AudioPluginChainSpec::Kind::ClipFx, "clip", "cutoff", .75, true});
    const auto events = copy(service);
    check(events.changed && events.scanned && events.error == service.error && events.notices.size() == 1 &&
        events.notices[0].kind == AudioPluginNotice::Kind::GestureBegin && events.notices[0].address.right &&
        events.notices[0].address.instance == 91 && events.notices[0].clipId == "clip" && events.notices[0].touch,
        "service notifications retain slot generation, side and clip ownership");
    AudioMiniModuleCompileRequest request;
    request.info = {48000, 128, 2, false};
    auto& target = request.targets.emplace_back(); target.address = {"master", "mini", false, 13};
    target.spec.id = "mini"; target.spec.uid = "daw.mini"; target.spec.profileSeed = 77;
    target.spec.parameters = {{"cutoff", .3, true}};
    const auto compiled = copy(request);
    check(compiled.info == request.info && compiled.targets[0].address.instance == 13 &&
        compiled.targets[0].spec.profileSeed == 77 && compiled.targets[0].spec.parameters[0].restoreAfterState,
        "mini compilation request is an owned specification with expected native identity");
    AudioMiniModuleCompileStatus status{AudioMiniModuleCompileStatus::State::Ready, {}, {{target.address, 3072}}};
    const auto ready = copy(status);
    check(ready.state == status.state && ready.targets[0].latency == 3072,
        "mini compilation status returns numeric target and latency values");

    auto badTable = wire::encode(plugins::slicer::SliceTable{});
    badTable[0] = 129;
    check(rejects([&] { (void)wire::decode<plugins::slicer::SliceTable>(badTable); }),
        "untrusted slicer counts are rejected before touching fixed storage");
    auto badNotice = wire::encode(service.notices[0]); badNotice[0] = 255;
    check(rejects([&] { (void)wire::decode<AudioPluginNotice>(badNotice); }),
        "unknown service event discriminants are rejected");
    PluginParameterReadout nonfinite; nonfinite.value = std::numeric_limits<double>::infinity();
    check(rejects([&] { (void)wire::encode(nonfinite); }), "plugin parameter values retain finite-number enforcement");
}
} // namespace

int main() try {
    metadataAndReadouts(); resourcesAndState(); servicesAndValidation();
    return failures ? 1 : 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
}
