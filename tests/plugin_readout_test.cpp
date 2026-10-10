#include "EngineController.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Internal/GravityInstance.hpp"
#include "Internal/ModulationRackInstance.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace {
int failures = 0;
bool check(bool condition, const char* message) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
    return condition;
}
}

int main() {
    namespace eq = daw::plugins::equalizer;
    namespace gravity = daw::plugins::gravity;
    namespace slicer = daw::plugins::slicer;
    using Rack = daw::plugins::modulation::ModulationRackInstance;
    std::optional<daw::SlicerSnapshot> retained;
    {
        auto owner = std::make_unique<daw::EngineController>();
        auto& c = *owner;
        if (!check(c.initialize(48000, 128, false).isOk(), "headless runtime initializes")) return 1;
        const auto track = c.addTrack(daw::TrackKind::Audio, "Readouts");
        const auto equalizer = c.addInsert(track, eq::EqualizerInstance::staticDescriptor());
        const auto gravitySlot = c.addInsert(track, gravity::GravityInstance::staticDescriptor());
        const auto rack = c.addInsert(track, Rack::staticDescriptor());
        check(!c.insertIdentity(track, "missing") && !c.slicerSnapshot(track, equalizer) &&
              !c.gravitySnapshot(track, equalizer) && !c.equalizerSnapshot(track, gravitySlot) &&
              !c.modulationResponse(track, equalizer), "missing or wrong processor returns no snapshot");

        const auto gain = eq::parameterId(eq::bandParameter(0, eq::BandParam::Gain));
        const auto enabled = eq::parameterId(eq::bandParameter(0, eq::BandParam::Enabled));
        c.setInsertParameter(track, equalizer, enabled, 1);
        c.setInsertParameter(track, equalizer, gain, 6);
        const auto editorInfo = c.insertEditorSnapshot(track, equalizer);
        check(editorInfo && editorInfo->identity == c.insertIdentity(track, equalizer) &&
              editorInfo->uid == "daw.equalizer" && !editorInfo->hasEditor &&
              !c.insertEditorSnapshot(track, "missing"), "editor metadata is an owned, typed snapshot");
        std::vector<daw::PluginParameterReadout> readings{
            {gain, 0}, {enabled, 100000}, {"unknown", 0}, {gain, -1}};
        const auto* storage = readings.data();
        c.readInsertParameters(track, equalizer, readings);
        check(readings.data() == storage && readings[0].available && readings[0].value == 6 &&
              readings[1].available && readings[1].value == 1 && !readings[2].available &&
              readings[2].index == -1 && readings[2].value == 0 && readings[3].value == 6 &&
              readings[3].index == readings[0].index,
              "parameter polling reuses storage and validates cached indices against stable IDs");
        const auto text = c.insertParameterText(track, equalizer, gain, 6, readings[0].index);
        check(!text.empty() && c.insertParameterText(track, equalizer, gain, 6, 0) == text &&
              c.insertParameterText(track, equalizer, "unknown", 6, 0).empty(),
              "native value formatting resolves stale hints and rejects unknown IDs");
        const auto captured = c.equalizerSnapshot(track, equalizer);
        if (!check(bool(captured), "equalizer snapshot available")) return 1;
        check(captured->bands[0].enabled && captured->bands[0].gainDb == 6,
              "snapshot observes a host edit before the next audio block");
        const auto depth = c.undoDepth();
        check(c.switchEqualizerComparison(track, equalizer, 'B') &&
              c.equalizerSnapshot(track, equalizer)->comparison == 'B' &&
              c.insertParameter(track, equalizer, gain) == 0 && c.undoDepth() == depth + 1,
              "A/B switches through one undoable parameter command group");
        c.undo();
        check(c.insertParameter(track, equalizer, gain) == 6, "Undo restores the audible EQ settings");
        c.redo();
        check(c.insertParameter(track, equalizer, gain) == 0, "Redo restores the comparison sound");
        check(c.switchEqualizerComparison(track, equalizer, 'A') &&
              c.insertParameter(track, equalizer, gain) == 6, "A retains its captured settings");
        check(!c.switchEqualizerComparison(track, equalizer, 'X'), "invalid comparison is rejected");
        check(c.copyEqualizerComparison(track, equalizer), "copy A/B succeeds");
        c.setInsertParameter(track, equalizer, gain, 9);
        c.switchEqualizerComparison(track, equalizer, 'B');
        check(c.insertParameter(track, equalizer, gain) == 6 && captured->bands[0].gainDb == 6,
              "copy is independent and earlier snapshots remain immutable");

        auto analyzer = captured->analyzer;
        analyzer.enabled = true; analyzer.sidechain = true; analyzer.frozen = true;
        check(c.setEqualizerAnalyzer(track, equalizer, analyzer) &&
              c.equalizerSnapshot(track, equalizer)->analyzer.frozen &&
              c.auditionEqualizerBand(track, equalizer, 0) &&
              c.auditionEqualizerBand(track, equalizer, -1), "analyzer and audition use controller commands");
        const auto response = c.equalizerResponse(track, equalizer);
        check(response && std::all_of(response->combined.begin(), response->combined.end(),
              [](double x) { return std::isfinite(x); }) &&
              *std::max_element(response->combined.begin(), response->combined.end()) > 5,
              "EQ response carries the audible peak without exposing the processor");
        check(c.modulationResponse(track, rack).has_value(), "rack supplies its EQ response by value");

        const auto presets = gravity::factoryPresets();
        check(c.setInsertPresetReference(track, gravitySlot, "factory", std::string(presets.back().name)) &&
              c.gravitySnapshot(track, gravitySlot)->lastPreset == int(presets.size()) - 1,
              "shared preset command updates Gravity's factory index");
        check(c.setGravityFrozen(track, gravitySlot, true) && c.gravitySnapshot(track, gravitySlot)->frozen &&
              c.clearGravityTail(track, gravitySlot), "Gravity freeze and tail clearing are routed commands");
        check(!c.setGravityFrozen(track, equalizer, true) && !c.clearGravityTail(track, equalizer) &&
              !c.setEqualizerAnalyzer(track, gravitySlot, analyzer), "commands reject the wrong processor type");

        check(c.setInsertChannelMode(track, equalizer, daw::PluginChannelMode::DualMono), "EQ enters dual mono");
        const auto left = c.insertIdentity(track, equalizer);
        c.setInsertEditorChannel(track, equalizer, daw::PluginEditorChannel::Right);
        const auto right = c.insertIdentity(track, equalizer);
        check(left && right && left != right, "identity follows the selected dual-mono processor");
        c.setInsertParameter(track, equalizer, gain, -4);
        check(c.equalizerSnapshot(track, equalizer)->bands[0].gainDb == -4, "readout follows right-channel edits");
        c.readInsertParameters(track, equalizer, readings);
        check(readings[0].value == -4 && readings[3].value == -4,
              "generic parameter polling follows the selected dual-mono side");
        check(!c.insertEditorSize(track, equalizer, left) &&
              !c.resizeInsertEditor(track, equalizer, left, {320, 200}) &&
              !c.openInsertEditor(track, equalizer, left) &&
              !c.pumpInsertEditor(track, equalizer, left),
              "native editor commands reject a different mono-side identity");
        check(c.closeInsertEditor(track, equalizer, left) && c.insertIdentity(track, equalizer) == right,
              "a left editor can detach after selecting right without changing selection");
        c.setInsertEditorChannel(track, equalizer, daw::PluginEditorChannel::Left);
        check(c.equalizerSnapshot(track, equalizer)->bands[0].gainDb == 6, "left-channel snapshot stays independent");

        const auto instrument = c.addTrack(daw::TrackKind::Instrument, "Slicer");
        if (!check(c.setTrackInstrumentPlugin(instrument, slicer::SlicerInstance::staticDescriptor()), "Slicer loads")) return 1;
        const auto slot = c.project().findTrack(instrument)->instrument.id;
        auto initial = c.slicerSnapshot(instrument, slot);
        if (!check(bool(initial), "Slicer snapshot available")) return 1;
        auto state = initial->state;
        state.audio = std::make_shared<daw::engine::SampleBuffer>(2, 512, 48000);
        auto table = std::make_shared<slicer::SliceTable>();
        table->frames = 512; table->count = 1; table->slices[0].end = 512; table->rebuild();
        state.table = table;
        check(c.applySlicerState(instrument, slot, state, "Fixture"), "controller accepts an immutable Slicer state");
        retained = c.slicerSnapshot(instrument, slot);
        const auto identity = retained->identity;
        check(c.beginSlicerEdit(instrument, slot), "Slicer transaction starts");
        c.setTrackInstrumentPlugin(instrument, {});
        check(!c.slicerSnapshot(instrument, slot) && !c.insertIdentity(instrument, slot), "removed Slicer has no live identity");
        c.undo();
        check(c.insertIdentity(instrument, slot) && c.insertIdentity(instrument, slot) != identity,
              "Undo recreates the same slot with a new instance identity");
        check(!c.updateSlicerEdit(table, state.analysis) && !c.commitSlicerEdit(),
              "a transaction cannot modify a recreated instrument");

        const auto oldProject = identity.project;
        c.newProject();
        check(!c.slicerSnapshot(instrument, slot), "new project invalidates old slot readouts");
        c.readInsertParameters(track, equalizer, readings);
        check(std::all_of(readings.begin(), readings.end(), [](const auto& value) {
                  return !value.available && value.index == -1 && value.value == 0;
              }) && c.insertParameterText(track, equalizer, gain, 6).empty() &&
              editorInfo->uid == "daw.equalizer" && editorInfo->identity,
              "removed slots invalidate polling while previously returned metadata stays owned");
        const auto freshTrack = c.addTrack(daw::TrackKind::Instrument, "Fresh Slicer");
        c.setTrackInstrumentPlugin(freshTrack, slicer::SlicerInstance::staticDescriptor());
        const auto freshSlot = c.project().findTrack(freshTrack)->instrument.id;
        check(c.insertIdentity(freshTrack, freshSlot).project != oldProject,
              "project replacement advances the identity generation");
    }
    check(retained && retained->state.audio && retained->state.audio->frames() == 512 &&
          retained->state.table && retained->state.table->slices[0].end == 512,
          "retained resources survive project replacement and runtime destruction");
    return failures ? 1 : 0;
}
