// Plugins in a real project: loaded into insert slots, heard in the mixdown,
// saved and reloaded with their state.
//
// Runs headless (`initialize(openDevice=false)`), so it needs no audio hardware.
// The fixture plugin computes out[i] = in[i-64] * gain + offset, which makes
// every assertion below something you can work out on paper.
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "recovery/RecoveryJournal.hpp"
#include "plugins/PluginConvert.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

namespace fs = std::filesystem;
using namespace audio;

static int failures = 0;
static bool check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++failures;
    return cond;
}

/// A steady full-scale-ish DC-free tone, so a gain change is a peak change.
static void writeTone(const std::string& path, SampleRate rate, BufferSize frames) {
    AudioBuffer tone(2, frames);
    for (BufferSize f = 0; f < frames; ++f) {
        const float s = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * float(f) / rate);
        tone.getChannel(0)[f] = s;
        tone.getChannel(1)[f] = s;
    }
    AudioRecorder rec;
    rec.initialize(rate, 2);
    rec.writeWAVFile(path, tone, rate);
}

static float peakOf(const std::string& path) {
    audio::platform::DecodedAudio decoded;
    if (!audio::platform::decodeAudioFile(path, decoded)) return -1.0f;
    float peak = 0.0f;
    for (float sample : decoded.interleaved) {
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

static float peakBetween(const std::string& path, double fromSeconds,
                         double toSeconds) {
    audio::platform::DecodedAudio decoded;
    if (!audio::platform::decodeAudioFile(path, decoded) || decoded.channels == 0)
        return -1.0f;
    const size_t from = std::min(size_t(decoded.frames),
        size_t(std::max(0.0, fromSeconds) * decoded.sampleRate));
    const size_t to = std::min(size_t(decoded.frames),
        size_t(std::max(fromSeconds, toSeconds) * decoded.sampleRate));
    float peak = 0.0f;
    for (size_t frame = from; frame < to; ++frame) {
        for (int channel = 0; channel < decoded.channels; ++channel) {
            peak = std::max(peak, std::fabs(
                decoded.interleaved[frame * size_t(decoded.channels) + size_t(channel)]));
        }
    }
    return peak;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string pluginPath = DAW_TEST_CLAP_PATH;
    const std::string scannerPath = DAW_SCAN_PATH;

    const fs::path dir = fs::temp_directory_path() / "daw_plugin_insert_test";
    std::error_code ec;
    // Some VST3 plugins expose placeholder parameters whose current value is
    // NaN. JSON spells that as null; it must not make the whole project
    // impossible to reopen.
    {
        daw::ProjectModel project;
        daw::TrackModel track;
        track.id = "track";
        daw::InsertModel insert;
        insert.id = "slot";
        insert.format = daw::PluginFormat::Vst3;
        insert.uid = "dxsplit";
        insert.parameters = {{"good", 0.25},
                             {"bad", std::numeric_limits<double>::quiet_NaN()}};
        track.inserts.push_back(std::move(insert));
        project.tracks.push_back(std::move(track));

        std::string encoded;
        const bool saved =
            daw::ProjectSerializer::serializeDocument(project, encoded).isOk();
        daw::ProjectModel decoded;
        const bool loaded = daw::ProjectSerializer::deserializeDocument(
            decoded,
            R"({"format":"vlt-project","tracks":[{"id":"track","inserts":[{"id":"slot","format":"vst3","uid":"dxsplit","parameters":[{"id":"bad","value":null},{"id":"good","value":0.25}]}]}]})")
                                .isOk();
        check(saved && encoded.find("\"id\":\"bad\"") == std::string::npos &&
                  loaded && decoded.tracks.size() == 1 &&
                  decoded.tracks.front().inserts.size() == 1 &&
                  decoded.tracks.front().inserts.front().parameters.size() == 1 &&
                  decoded.tracks.front().inserts.front().parameters.front().id ==
                      "good",
              "non-finite plugin parameters neither corrupt nor block a project");
    }

    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    const std::string tonePath = (dir / "tone.wav").string();
    writeTone(tonePath, 48000, 48000);

    daw::EngineController ctrl;
    check(ctrl.initialize(48000, 512, /*openDevice=*/false).isOk(),
          "the controller initialises without a device");

    // Point the manager at the fixture and scan it.
    ctrl.pluginManager().setScannerPath(scannerPath);
    ctrl.pluginManager().setSearchPaths(
        daw::plugins::Format::Clap,
        {fs::path(pluginPath).parent_path().string()});
    ctrl.pluginManager().setSearchPaths(daw::plugins::Format::Vst3, {});
    ctrl.pluginManager().setSearchPaths(daw::plugins::Format::AudioUnit, {});
    ctrl.pluginManager().startScan();
    ctrl.pluginManager().waitForScan();

    const auto descriptor =
        ctrl.pluginManager().find(daw::plugins::Format::Clap, "com.daw.test.gain");
    if (!check(descriptor.has_value(), "the manager found the fixture plugin")) {
        std::printf("\nFAILURES PRESENT\n");
        return 1;
    }
    const daw::plugins::PluginDescriptor found = *descriptor;

    const std::string trackId = ctrl.addTrack(daw::TrackKind::Audio, "Audio");
    const std::string clipId = ctrl.importAudio(tonePath, trackId, 0.0);

    std::string gravityTrackId;
    std::string gravityInsertId;
    daw::AutomationTarget gravityAutomation;
    std::string graphitTrackId;
    std::string graphitInsertId;
    daw::AutomationTarget graphitAutomation;
    daw::AutomationTarget graphitPriorityAutomation;
    // ── Built-in Gravity follows the ordinary insert/project pipeline ──
    {
        const auto gravity = ctrl.pluginManager().find(
            daw::plugins::Format::Internal, "daw.gravity");
        check(gravity.has_value() && !gravity->isInstrument,
              "Gravity appears in the built-in effect catalogue");
        gravityTrackId = ctrl.addTrack(daw::TrackKind::Audio, "Gravity Test");
        gravityInsertId = gravity
            ? ctrl.addInsert(gravityTrackId, *gravity)
            : std::string{};
        check(!gravityInsertId.empty() &&
                  ctrl.insertInstance(gravityTrackId, gravityInsertId) != nullptr,
              "Gravity adds as a live insert");
        ctrl.setInsertParameter(gravityTrackId, gravityInsertId, "pitch", 4.5);
        ctrl.setInsertParameter(gravityTrackId, gravityInsertId, "mass", 0.8);
        ctrl.setInsertParameter(gravityTrackId, gravityInsertId,
                                "detector.source", 1.0);
        check(ctrl.insertSupportsSidechain(gravityTrackId, gravityInsertId),
              "Gravity exposes its optional detector sidechain");

        gravityAutomation.kind = daw::AutomationTargetKind::PluginParameter;
        gravityAutomation.channelId = gravityTrackId;
        gravityAutomation.slotId = gravityInsertId;
        gravityAutomation.parameterId = "gravity";
        const auto automation = ctrl.ensureAutomation(gravityAutomation);
        check(!automation.first.empty() && !automation.second.empty(),
              "Gravity parameters can create automation");

        const std::string duplicate = ctrl.duplicateTrack(gravityTrackId, true);
        const std::vector<daw::InsertModel>* copied = ctrl.channelInserts(duplicate);
        check(copied && copied->size() == 1 &&
                  copied->front().uid == "daw.gravity" &&
                  copied->front().id != gravityInsertId &&
                  std::fabs(ctrl.insertParameter(duplicate, copied->front().id,
                                                 "pitch") - 4.5) < 1e-6 &&
                  std::fabs(ctrl.insertParameter(duplicate, copied->front().id,
                                                 "mass") - 0.8) < 1e-6,
              "duplicating a track clones Gravity with independent state");
        ctrl.removeTrack(duplicate);
    }

    // ── Built-in Graphit follows the same state and automation pipeline ──
    {
        const auto graphit = ctrl.pluginManager().find(
            daw::plugins::Format::Internal, "daw.graphit");
        check(graphit.has_value() && !graphit->isInstrument,
              "Graphit appears in the built-in effect catalogue");
        graphitTrackId = ctrl.addTrack(daw::TrackKind::Audio, "Graphit Test");
        graphitInsertId = graphit
            ? ctrl.addInsert(graphitTrackId, *graphit)
            : std::string{};
        check(!graphitInsertId.empty() &&
                  ctrl.insertInstance(graphitTrackId, graphitInsertId) != nullptr,
              "Graphit adds as a live insert");
        ctrl.setInsertParameter(graphitTrackId, graphitInsertId, "amount", 0.82);
        ctrl.setInsertParameter(graphitTrackId, graphitInsertId, "mode", 3.0);
        ctrl.setInsertParameter(graphitTrackId, graphitInsertId, "priority", 0.45);

        graphitAutomation.kind = daw::AutomationTargetKind::PluginParameter;
        graphitAutomation.channelId = graphitTrackId;
        graphitAutomation.slotId = graphitInsertId;
        graphitAutomation.parameterId = "amount";
        const auto automation = ctrl.ensureAutomation(graphitAutomation);
        check(!automation.first.empty() && !automation.second.empty(),
              "Graphit Amount can create automation");
        graphitPriorityAutomation = graphitAutomation;
        graphitPriorityAutomation.parameterId = "priority";
        const auto priorityAutomation =
            ctrl.ensureAutomation(graphitPriorityAutomation);
        check(!priorityAutomation.first.empty() &&
                  !priorityAutomation.second.empty(),
              "Graphit Priority can create automation");

        const std::string duplicate = ctrl.duplicateTrack(graphitTrackId, true);
        const std::vector<daw::InsertModel>* copied = ctrl.channelInserts(duplicate);
        check(copied && copied->size() == 1 &&
                  copied->front().uid == "daw.graphit" &&
                  copied->front().id != graphitInsertId &&
                  std::fabs(ctrl.insertParameter(duplicate, copied->front().id,
                                                 "amount") - 0.82) < 1e-6 &&
                  ctrl.insertParameter(duplicate, copied->front().id, "mode") == 3.0 &&
                  std::fabs(ctrl.insertParameter(duplicate, copied->front().id,
                                                 "priority") - 0.45) < 1e-6,
              "duplicating a track clones independent Graphit state");
        ctrl.removeTrack(duplicate);
    }

    const std::string dryPath = (dir / "dry.wav").string();
    check(ctrl.exportMixdown(dryPath, false).isOk(), "exports the dry mixdown");
    const float dryPeak = peakOf(dryPath);
    check(dryPeak > 0.1f, "the dry mixdown has signal");

    // ── Clip-owned FX use the same strip without becoming track FX ──
    std::string clipNestedId;
    {
        daw::plugins::PluginDescriptor missingFx = found;
        missingFx.uid = "com.daw.missing.clip-fx";
        check(ctrl.addClipFxInsert(trackId, clipId, missingFx).empty() &&
                  ctrl.clipFx(trackId, clipId)->empty(),
              "a missing clip effect fails without leaving a dead slot");

        clipNestedId = ctrl.addClipFxInsert(trackId, clipId, found);
        check(!clipNestedId.empty() && ctrl.clipFx(trackId, clipId)->size() == 1,
              "an audio clip owns an independent insert chain");
        check(ctrl.channelInserts(trackId)->empty(),
              "clip FX do not appear in the track insert chain");
        const auto parameters = ctrl.insertParameters(trackId, clipNestedId);
        ctrl.setInsertParameter(trackId, clipNestedId, parameters.front().id, 0.25);

        // A second clip from the same WAV remains dry. Measuring non-overlapping
        // time ranges proves the private plugin hears only its owner.
        const std::string drySibling = ctrl.importAudio(tonePath, trackId, 1.25);
        const std::string scopedPath = (dir / "clip_scoped.wav").string();
        check(ctrl.exportMixdown(scopedPath, false).isOk(),
              "a project with clip FX renders");
        const float wetRegion = peakBetween(scopedPath, 0.1, 0.9);
        const float siblingRegion = peakBetween(scopedPath, 1.35, 2.1);
        check(std::fabs(wetRegion - dryPeak * 0.25f) < 0.025f &&
                  std::fabs(siblingRegion - dryPeak) < 0.025f,
              "clip FX process their clip but bypass a sibling from the same WAV");

        ctrl.setClipFxVolume(trackId, clipId, 1.25f);
        ctrl.setClipFxPan(trackId, clipId, -0.4f);
        const daw::ClipModel* edited = ctrl.audioClip(trackId, clipId);
        check(edited && std::fabs(edited->gain - 1.25f) < 1e-6f &&
                  std::fabs(edited->pan + 0.4f) < 1e-6f,
              "the Clip strip owns post-FX Volume and Pan");

        const std::string copiedId = ctrl.duplicateClip(trackId, clipId);
        const daw::ClipModel* copied = ctrl.audioClip(trackId, copiedId);
        check(copied && copied->inserts.size() == 1 &&
                  copied->inserts.front().id != clipNestedId,
              "duplicating a clip clones private FX with fresh slot ids");
        ctrl.undo();
        check(ctrl.audioClip(trackId, copiedId) == nullptr,
              "undo removes the duplicated private clip chain");

        // Keep the sibling for later signal-path assertions; its dry peak makes
        // the existing track-FX checks independent of the clip effect above.
        check(ctrl.audioClip(trackId, drySibling) != nullptr,
              "the dry sibling remains a normal timeline clip");
    }

    std::string samplerTrackId;
    std::string samplerSlotId;
    std::string samplerNestedId;
    // ── Sampler-owned FX are a separate, bounded graph stage ──
    {
        samplerTrackId =
            ctrl.addTrack(daw::TrackKind::Instrument, "Sampler FX Test");
        const auto sampler = ctrl.pluginManager().find(
            daw::plugins::Format::Internal, "daw.sampler");
        check(sampler.has_value() &&
                  ctrl.setTrackInstrumentPlugin(samplerTrackId, *sampler),
              "the built-in sampler loads for scoped FX testing");
        const daw::TrackModel* track = ctrl.project().findTrack(samplerTrackId);
        samplerSlotId = track ? track->instrument.id : std::string{};
        check(track && track->samplerFx.ownerInstrumentId == samplerSlotId,
              "the sampler strip is owned by that sampler instance");

        daw::plugins::PluginDescriptor missingFx = found;
        missingFx.uid = "com.daw.missing.sampler-fx";
        check(ctrl.addSamplerFxInsert(samplerTrackId, samplerSlotId, missingFx).empty() &&
                  ctrl.samplerFx(samplerTrackId, samplerSlotId)->inserts.empty(),
              "a missing sampler effect fails without leaving a dead slot");

        std::vector<std::string> nested;
        for (std::size_t i = 0; i < daw::EngineController::kSamplerFxSlots; ++i) {
            nested.push_back(ctrl.addSamplerFxInsert(
                samplerTrackId, samplerSlotId, found));
        }
        check(std::all_of(nested.begin(), nested.end(),
                          [](const std::string& id) { return !id.empty(); }),
              "all eight sampler FX slots accept effects");
        samplerNestedId = nested.front();
        check(ctrl.addSamplerFxInsert(samplerTrackId, samplerSlotId, found).empty(),
              "a ninth sampler FX is rejected safely");
        const auto* nodes = ctrl.trackNodes(samplerTrackId);
        check(nodes && nodes->samplerInserts.size() == 8 &&
                  nodes->samplerFader != daw::engine::kInvalidNode &&
                  nodes->samplerMeter != daw::engine::kInvalidNode,
              "the graph has sampler inserts, post-FX fader and meter");
        check(nodes && nodes->inserts.empty(),
              "sampler FX do not leak into the ordinary track insert list");

        const std::string copiedId = ctrl.duplicateTrack(samplerTrackId, true);
        const daw::TrackModel* copied = ctrl.project().findTrack(copiedId);
        check(copied && copied->samplerFx.inserts.size() == 8 &&
                  copied->samplerFx.ownerInstrumentId == copied->instrument.id &&
                  copied->samplerFx.inserts.front().id != nested.front(),
              "duplicating a track clones sampler FX with independent slot ids");
        ctrl.removeTrack(copiedId);

        ctrl.setSamplerFxVolume(samplerTrackId, samplerSlotId, 1.5f);
        ctrl.setSamplerFxPan(samplerTrackId, samplerSlotId, -0.25f);
        const daw::SamplerFxModel* fx =
            ctrl.samplerFx(samplerTrackId, samplerSlotId);
        check(fx && std::fabs(fx->volume - 1.5f) < 1e-6f &&
                  std::fabs(fx->pan + 0.25f) < 1e-6f,
              "sampler post-FX volume and pan update independently");
        ctrl.setInsertMix(samplerTrackId, nested.front(), 0.35f);
        ctrl.commitInsertMixEdit(samplerTrackId, nested.front(), 1.0f,
                                 "Sampler FX Mix");
        ctrl.undo();
        check(std::fabs(ctrl.samplerFx(samplerTrackId, samplerSlotId)
                            ->inserts.front().mix - 1.0f) < 1e-6f,
              "nested dry/wet participates in undo");
        ctrl.redo();
        ctrl.setInsertBypassed(samplerTrackId, nested.front(), true);
        const auto nestedParameters =
            ctrl.insertParameters(samplerTrackId, nested.front());
        ctrl.setInsertParameter(samplerTrackId, nested.front(),
                                nestedParameters.front().id, 0.42);
        fx = ctrl.samplerFx(samplerTrackId, samplerSlotId);
        check(fx && std::fabs(fx->inserts.front().mix - 0.35f) < 1e-6f &&
                  fx->inserts.front().bypassed,
              "generic slot APIs address nested dry/wet and bypass by id");
        check(ctrl.loadSamplerSample(samplerTrackId, samplerSlotId, tonePath) &&
                  ctrl.samplerFx(samplerTrackId, samplerSlotId)->inserts.size() == 8,
              "loading another sample keeps the sampler FX chain");

        ctrl.removeSamplerFxInsert(samplerTrackId, samplerSlotId, nested.back());
        check(ctrl.samplerFx(samplerTrackId, samplerSlotId)->inserts.size() == 7,
              "a nested effect removes");
        ctrl.undo();
        check(ctrl.samplerFx(samplerTrackId, samplerSlotId)->inserts.size() == 8,
              "undo restores the nested effect");

        const auto toneInstrument = ctrl.pluginManager().find(
            daw::plugins::Format::Clap, "com.daw.test.tone");
        check(toneInstrument && ctrl.setTrackInstrumentPlugin(
                                    samplerTrackId, *toneInstrument),
              "the sampler can be replaced by another instrument");
        const daw::TrackModel* replaced = ctrl.project().findTrack(samplerTrackId);
        check(replaced && replaced->samplerFx.inserts.empty() &&
                  replaced->samplerFx.ownerInstrumentId.empty(),
              "replacing the sampler clears its private FX chain");
        ctrl.undo();
        replaced = ctrl.project().findTrack(samplerTrackId);
        check(replaced && replaced->instrument.uid == "daw.sampler" &&
                  replaced->samplerFx.inserts.size() == 8,
              "undo restores the sampler and its entire FX chain");
    }

    // ── Loading a plugin into a slot ──
    std::string insertId;
    {
        insertId = ctrl.addInsert(trackId, found);
        check(!insertId.empty(), "a plugin loads into an insert slot");

        const std::vector<daw::InsertModel>* slots = ctrl.channelInserts(trackId);
        check(slots && slots->size() == 1, "the document records the slot");
        check(slots && slots->front().isLoaded() &&
                  slots->front().uid == "com.daw.test.gain",
              "the slot refers to the plugin by identity");

        const daw::EngineController::TrackNodes* nodes = ctrl.trackNodes(trackId);
        check(nodes && nodes->inserts.size() == 1,
              "the compiled graph has the insert in the chain");
        check(nodes && nodes->preFaderTap == nodes->inserts.back(),
              "a pre-fader send taps after the inserts, not before them");
    }

    // ── The plugin is actually in the signal path ──
    {
        const std::vector<daw::plugins::ParameterInfo> parameters =
            ctrl.insertParameters(trackId, insertId);
        check(parameters.size() == 2, "the slot exposes the plugin's parameters");

        // Halve the gain and the mixdown must halve with it.
        ctrl.setInsertParameter(trackId, insertId, parameters[0].id, 0.5);
        const std::string halfPath = (dir / "half.wav").string();
        check(ctrl.exportMixdown(halfPath, false).isOk(), "exports through the plugin");
        const float halfPeak = peakOf(halfPath);
        check(std::fabs(halfPeak - dryPeak * 0.5f) < 0.02f,
              "the plugin's gain is heard in the mixdown");

        ctrl.setInsertMix(trackId, insertId, 0.5f);
        const std::string mixedPath = (dir / "mixed.wav").string();
        check(ctrl.exportMixdown(mixedPath, false).isOk(), "exports at 50% wet");
        check(std::fabs(peakOf(mixedPath) - dryPeak * 0.75f) < 0.02f,
              "insert Mix blends the processed and dry signals");
        ctrl.setInsertMix(trackId, insertId, 1.0f);

        // Bypass must bring the dry signal back.
        ctrl.setInsertBypassed(trackId, insertId, true);
        const std::string bypassPath = (dir / "bypass.wav").string();
        check(ctrl.exportMixdown(bypassPath, false).isOk(), "exports bypassed");
        check(std::fabs(peakOf(bypassPath) - dryPeak) < 0.02f,
              "a bypassed plugin passes the dry signal through");
        ctrl.setInsertBypassed(trackId, insertId, false);
    }

    // ── Host wrapper routing: explicit modes, dual mono and sidechain ──
    {
        check(ctrl.insertSupportsSidechain(trackId, insertId),
              "the controller sees the fixture's auxiliary input bus");
        const auto sources = ctrl.insertSidechainSources(trackId);
        check(std::any_of(sources.begin(), sources.end(), [&](const auto& source) {
                  return source.id == samplerTrackId;
              }),
              "the wrapper offers another audio-capable track as sidechain");
        check(!ctrl.setInsertSidechainSource(trackId, insertId, trackId),
              "a self-sidechain is rejected before it can close the graph loop");
        check(ctrl.setInsertSidechainSource(trackId, insertId, samplerTrackId) &&
                  ctrl.insertModel(trackId, insertId)->sidechainTrackId ==
                      samplerTrackId,
              "a safe sidechain route is saved on the slot");

        check(ctrl.setInsertChannelMode(trackId, insertId,
                                        daw::PluginChannelMode::DualMono),
              "Dual Mono creates two independent effect instances");
        const auto parameters = ctrl.insertParameters(trackId, insertId);
        ctrl.setInsertEditorChannel(trackId, insertId,
                                    daw::PluginEditorChannel::Left);
        ctrl.setInsertParameter(trackId, insertId, parameters[0].id, 0.25);
        ctrl.setInsertEditorChannel(trackId, insertId,
                                    daw::PluginEditorChannel::Right);
        ctrl.setInsertParameter(trackId, insertId, parameters[0].id, 0.75);
        ctrl.setInsertEditorChannel(trackId, insertId,
                                    daw::PluginEditorChannel::Left);
        const daw::InsertModel* dualModel = ctrl.insertModel(trackId, insertId);
        const auto storedValue = [&](const std::vector<daw::InsertParameter>& values) {
            const auto found = std::find_if(
                values.begin(), values.end(), [&](const auto& parameter) {
                    return parameter.id == parameters[0].id;
                });
            return found == values.end() ? -1.0 : found->value;
        };
        const double left = dualModel ? storedValue(dualModel->parameters) : -1.0;
        const double right =
            dualModel ? storedValue(dualModel->rightParameters) : -1.0;
        check(std::fabs(left - 0.25) < 1e-6 &&
                  std::fabs(right - 0.75) < 1e-6,
              "the L/R selector addresses independent dual-mono parameter state");

        // Return to the gain used by the signal-path assertions below, but
        // leave R selected so project persistence covers the editor choice.
        ctrl.setInsertParameter(trackId, insertId, parameters[0].id, 0.5);
        ctrl.setInsertEditorChannel(trackId, insertId,
                                    daw::PluginEditorChannel::Right);
        ctrl.setInsertParameter(trackId, insertId, parameters[0].id, 0.5);
    }

    // ── Order matters ──
    //
    // The plugin applies gain then offset, so two of them in series are not
    // commutative: (x*g1+o1)*g2+o2 differs from (x*g2+o2)*g1+o1.
    {
        const std::string secondId = ctrl.addInsert(trackId, found);
        check(!secondId.empty(), "a second plugin loads into the same channel");

        const std::vector<daw::plugins::ParameterInfo> parameters =
            ctrl.insertParameters(trackId, secondId);
        ctrl.setInsertParameter(trackId, secondId, parameters[1].id, 0.25);  // offset

        const std::string orderA = (dir / "order_a.wav").string();
        ctrl.exportMixdown(orderA, false);
        const float peakA = peakOf(orderA);

        ctrl.moveInsert(trackId, secondId, 0);
        const daw::EngineController::TrackNodes* nodes = ctrl.trackNodes(trackId);
        check(nodes && nodes->inserts.size() == 2, "both inserts are in the chain");

        const std::string orderB = (dir / "order_b.wav").string();
        ctrl.exportMixdown(orderB, false);
        const float peakB = peakOf(orderB);

        check(std::fabs(peakA - peakB) > 0.01f,
              "reordering the inserts changes what comes out");

        ctrl.removeInsert(trackId, secondId);
        check(ctrl.channelInserts(trackId)->size() == 1, "an insert can be removed");
    }

    // ── Replacing the plugin in a slot ──
    //
    // The slot id survives, because automation lanes and any open editor window
    // are keyed on it. Everything else about the slot must become the new
    // plugin — and, most of all, there must *be* a live plugin behind it
    // afterwards: a replace that leaves the slot dead looks to the user like a
    // plugin that "will not even open".
    {
        const std::string slotId = ctrl.addInsert(trackId, found);
        check(!slotId.empty(), "a plugin loads to be replaced");

        daw::plugins::PluginDescriptor other = found;
        other.uid = "com.daw.test.tone";
        other.name = "Test Tone";
        other.isInstrument = true;

        ctrl.replaceInsert(trackId, slotId, other);
        const daw::InsertModel* slot = nullptr;
        for (const daw::InsertModel& s : *ctrl.channelInserts(trackId))
            if (s.id == slotId) slot = &s;
        check(slot && slot->uid == "com.daw.test.tone",
              "the document slot now names the new plugin");
        check(slot && slot->id == slotId,
              "and keeps its id, so lanes and windows still address it");
        check(ctrl.insertInstance(trackId, slotId) != nullptr,
              "a live plugin stands behind the slot after a replace");
        check(ctrl.insertInstance(trackId, slotId) &&
                  ctrl.insertInstance(trackId, slotId)->descriptor().uid ==
                      "com.daw.test.tone",
              "and it is the plugin that was asked for");
        const daw::EngineController::TrackNodes* afterNodes = ctrl.trackNodes(trackId);
        check(afterNodes && afterNodes->inserts.size() == 2,
              "the chain still has both slots wired");

        ctrl.undo();
        check(ctrl.insertInstance(trackId, slotId) &&
                  ctrl.insertInstance(trackId, slotId)->descriptor().uid ==
                      "com.daw.test.gain",
              "undo puts the original plugin back, loaded");
        ctrl.redo();
        check(ctrl.insertInstance(trackId, slotId) &&
                  ctrl.insertInstance(trackId, slotId)->descriptor().uid ==
                      "com.daw.test.tone",
              "redo swaps it again, loaded");

        // A replace to something that cannot be instantiated must not eat the
        // plugin that was working.
        daw::plugins::PluginDescriptor missing = found;
        missing.uid = "com.daw.missing.effect";
        ctrl.replaceInsert(trackId, slotId, missing);
        check(ctrl.insertInstance(trackId, slotId) != nullptr,
              "a replace with a plugin that cannot load keeps the old one");

        ctrl.removeInsert(trackId, slotId);
        check(ctrl.channelInserts(trackId)->size() == 1,
              "the replaced slot removes cleanly");
    }

    // ── Undo and redo ──
    {
        const size_t before = ctrl.channelInserts(trackId)->size();
        const std::string temporary = ctrl.addInsert(trackId, found);
        check(ctrl.channelInserts(trackId)->size() == before + 1, "insert added");
        ctrl.undo();
        check(ctrl.channelInserts(trackId)->size() == before, "undo removes it");
        ctrl.redo();
        check(ctrl.channelInserts(trackId)->size() == before + 1, "redo puts it back");
        ctrl.removeInsert(trackId, temporary);
    }

    // ── Master inserts ──
    {
        const std::string masterInsert = ctrl.addInsert(
            std::string(daw::EngineController::kMasterChannelId), found);
        check(!masterInsert.empty(), "a plugin loads onto the master bus");
        check(ctrl.project().masterInserts.size() == 1,
              "the master's slot is in the document");

        const std::vector<daw::plugins::ParameterInfo> parameters = ctrl.insertParameters(
            std::string(daw::EngineController::kMasterChannelId), masterInsert);
        ctrl.setInsertParameter(std::string(daw::EngineController::kMasterChannelId),
                                masterInsert, parameters[0].id, 0.25);

        const std::string masterPath = (dir / "master.wav").string();
        ctrl.exportMixdown(masterPath, false);
        // The track insert is still at 0.5 gain, and the master one at 0.25.
        check(std::fabs(peakOf(masterPath) - dryPeak * 0.5f * 0.25f) < 0.02f,
              "a master insert processes the whole mix");

        ctrl.removeInsert(std::string(daw::EngineController::kMasterChannelId),
                          masterInsert);
    }

    // ── Crash recovery keeps fresh opaque state without a manual save ──
    {
        const auto parameters = ctrl.insertParameters(trackId, insertId);
        ctrl.setInsertEditorChannel(trackId, insertId,
                                    daw::PluginEditorChannel::Left);
        ctrl.setInsertParameter(trackId, insertId, parameters.front().id, 0.37);
        ctrl.setInsertEditorChannel(trackId, insertId,
                                    daw::PluginEditorChannel::Right);
        ctrl.setInsertParameter(trackId, insertId, parameters.front().id, 0.73);
        check(ctrl.loadSamplerSample(samplerTrackId, samplerSlotId, tonePath),
              "the Sampler has content immediately before recovery capture");
        // Host parameter writes are timestamped events. Render one block range
        // so the fixture's opaque processor state contains the new L/R values;
        // the inline parameter fallback is deliberately removed below.
        check(ctrl.exportMixdown((dir / "before_recovery.wav").string(), false)
                  .isOk(),
              "queued plugin values reach DSP before the crash snapshot");

        daw::recovery::RecoverySnapshot snapshot =
            ctrl.captureRecoverySnapshot();
        const std::vector<std::string> preferredRecoveryState{insertId};
        check(!ctrl.refreshRecoveryPluginStates(1, preferredRecoveryState),
              "a bounded recovery poll reuses an unchanged opaque state");
        ctrl.setInsertEditorChannel(trackId, insertId,
                                    daw::PluginEditorChannel::Left);
        ctrl.setInsertParameter(trackId, insertId, parameters.front().id, 0.41);
        check(ctrl.exportMixdown((dir / "recovery_state_probe.wav").string(),
                                 false).isOk(),
              "the recovery state probe reaches the plugin processor");
        check(ctrl.refreshRecoveryPluginStates(1, preferredRecoveryState),
              "a preferred editor state reports a real content change");
        check(!ctrl.refreshRecoveryPluginStates(1, preferredRecoveryState),
              "the same opaque state does not request another journal write");
        auto clearFallbacks = [](std::vector<daw::InsertModel>& slots) {
            for (daw::InsertModel& slot : slots) {
                slot.parameters.clear();
                slot.rightParameters.clear();
            }
        };
        for (daw::TrackModel& track : snapshot.project.tracks) {
            track.instrument.parameters.clear();
            track.instrument.rightParameters.clear();
            clearFallbacks(track.samplerFx.inserts);
            for (daw::ClipModel& clip : track.clips)
                clearFallbacks(clip.inserts);
            clearFallbacks(track.inserts);
        }
        clearFallbacks(snapshot.project.masterInserts);
        check(snapshot.pluginStates.size() >= 12,
              "recovery captures instruments, sampler FX, clip FX and both Dual Mono halves");

        const fs::path recoveryRoot = dir / "recovery";
        daw::recovery::RecoveryJournal journal;
        check(journal.start(recoveryRoot.string(), "plugin-test",
                            std::chrono::milliseconds(10)),
              "the recovery journal starts for a plugin project");
        const std::string sessionDir = journal.sessionDir();
        journal.requestWrite(std::move(snapshot));
        journal.flush();

        daw::ProjectModel recoveredModel;
        check(daw::ProjectSerializer::loadDocument(
                  recoveredModel,
                  (fs::path(sessionDir) / "project.json").string(), "").isOk(),
              "the plugin recovery manifest loads");

        daw::EngineController recovered;
        recovered.initialize(48000, 512, /*openDevice=*/false);
        recovered.pluginManager().setScannerPath(scannerPath);
        recovered.pluginManager().setSearchPaths(
            daw::plugins::Format::Clap,
            {fs::path(pluginPath).parent_path().string()});
        recovered.pluginManager().setSearchPaths(daw::plugins::Format::Vst3, {});
        recovered.pluginManager().setSearchPaths(
            daw::plugins::Format::AudioUnit, {});
        recovered.pluginManager().startScan();
        recovered.pluginManager().waitForScan();
        check(recovered.restoreRecoveryProject(
                  std::move(recoveredModel), sessionDir).isOk(),
              "a crash journal activates through the full plugin-state load path");

        recovered.setInsertEditorChannel(trackId, insertId,
                                         daw::PluginEditorChannel::Left);
        const double recoveredLeft = recovered.insertParameter(
            trackId, insertId, parameters.front().id);
        recovered.setInsertEditorChannel(trackId, insertId,
                                         daw::PluginEditorChannel::Right);
        const double recoveredRight = recovered.insertParameter(
            trackId, insertId, parameters.front().id);
        check(std::fabs(recoveredLeft - 0.37) < 1e-6 &&
                  std::fabs(recoveredRight - 0.73) < 1e-6,
              "both Dual Mono instances restore from recovery chunks, not parameter fallback");

        auto* recoveredSampler = dynamic_cast<daw::plugins::sampler::SamplerInstance*>(
            recovered.insertInstance(samplerTrackId, samplerSlotId));
        check(recoveredSampler && recoveredSampler->rawSample() &&
                  recoveredSampler->samplePath() == tonePath,
              "the built-in Sampler restores its sample and internal state from recovery");
        const auto recoveredClipParameters =
            recovered.insertParameters(trackId, clipNestedId);
        const auto recoveredSamplerFxParameters =
            recovered.insertParameters(samplerTrackId, samplerNestedId);
        check(!recoveredClipParameters.empty() &&
                  !recoveredSamplerFxParameters.empty() &&
                  std::fabs(recovered.insertParameter(
                                trackId, clipNestedId,
                                recoveredClipParameters.front().id) - 0.25) < 1e-6 &&
                  std::fabs(recovered.insertParameter(
                                samplerTrackId, samplerNestedId,
                                recoveredSamplerFxParameters.front().id) - 0.42) < 1e-6,
              "clip and sampler-owned effect state also survives crash recovery");
        journal.stop();

        // Leave the controller in the values expected by the ordinary Save
        // round-trip below.
        ctrl.setInsertEditorChannel(trackId, insertId,
                                    daw::PluginEditorChannel::Left);
        ctrl.setInsertParameter(trackId, insertId, parameters.front().id, 0.5);
        ctrl.setInsertEditorChannel(trackId, insertId,
                                    daw::PluginEditorChannel::Right);
        ctrl.setInsertParameter(trackId, insertId, parameters.front().id, 0.5);
        ctrl.exportMixdown((dir / "after_recovery_reset.wav").string(), false);
    }

    // ── Save and reload keeps the plugin and its state ──
    {
        const std::string packageDir = (dir / "Project.vlt").string();
        check(ctrl.saveProject(packageDir).isOk(), "the project saves");
        check(fs::exists(fs::path(packageDir) / "State"),
              "the package has a state folder");

        // The chunk is a file next to the JSON, not inline in it.
        bool sawChunk = false;
        for (const auto& entry : fs::directory_iterator(fs::path(packageDir) / "State", ec)) {
            if (entry.is_regular_file(ec)) sawChunk = true;
        }
        check(sawChunk, "the plugin's state was written as a chunk file");

        const std::string beforePath = (dir / "before_reload.wav").string();
        ctrl.exportMixdown(beforePath, false);
        const float beforePeak = peakOf(beforePath);

        daw::EngineController reloaded;
        reloaded.initialize(48000, 512, /*openDevice=*/false);
        reloaded.pluginManager().setScannerPath(scannerPath);
        reloaded.pluginManager().setSearchPaths(
            daw::plugins::Format::Clap,
            {fs::path(pluginPath).parent_path().string()});
        reloaded.pluginManager().setSearchPaths(daw::plugins::Format::Vst3, {});
        reloaded.pluginManager().setSearchPaths(daw::plugins::Format::AudioUnit, {});
        reloaded.pluginManager().startScan();
        reloaded.pluginManager().waitForScan();

        check(reloaded.openProject(packageDir).isOk(), "the project reloads");
        const std::vector<daw::InsertModel>* reloadedGravity =
            reloaded.channelInserts(gravityTrackId);
        check(reloadedGravity && reloadedGravity->size() == 1 &&
                  reloadedGravity->front().uid == "daw.gravity" &&
                  std::fabs(reloaded.insertParameter(
                                gravityTrackId, reloadedGravity->front().id,
                                "pitch") - 4.5) < 1e-6 &&
                  std::fabs(reloaded.insertParameter(
                                gravityTrackId, reloadedGravity->front().id,
                                "mass") - 0.8) < 1e-6 &&
                  std::fabs(reloaded.insertParameter(
                                gravityTrackId, reloadedGravity->front().id,
                                "detector.source") - 1.0) < 1e-6,
              "Gravity insert and advanced parameters survive project reload");
        const auto reloadedGravityAutomation =
            reloaded.findAutomation(gravityAutomation);
        check(!reloadedGravityAutomation.first.empty() &&
                  !reloadedGravityAutomation.second.empty(),
              "Gravity automation target survives project reload");
        const std::vector<daw::InsertModel>* reloadedGraphit =
            reloaded.channelInserts(graphitTrackId);
        check(reloadedGraphit && reloadedGraphit->size() == 1 &&
                  reloadedGraphit->front().uid == "daw.graphit" &&
                  std::fabs(reloaded.insertParameter(
                                graphitTrackId, reloadedGraphit->front().id,
                                "amount") - 0.82) < 1e-6 &&
                  reloaded.insertParameter(
                      graphitTrackId, reloadedGraphit->front().id, "mode") == 3.0 &&
                  std::fabs(reloaded.insertParameter(
                                graphitTrackId, reloadedGraphit->front().id,
                                "priority") - 0.45) < 1e-6,
              "Graphit insert and parameters survive project reload");
        const auto reloadedGraphitAutomation =
            reloaded.findAutomation(graphitAutomation);
        check(!reloadedGraphitAutomation.first.empty() &&
                  !reloadedGraphitAutomation.second.empty(),
              "Graphit automation target survives project reload");
        const auto reloadedGraphitPriorityAutomation =
            reloaded.findAutomation(graphitPriorityAutomation);
        check(!reloadedGraphitPriorityAutomation.first.empty() &&
                  !reloadedGraphitPriorityAutomation.second.empty(),
              "Graphit Priority automation survives project reload");
        const std::vector<daw::InsertModel>* slots =
            reloaded.channelInserts(reloaded.project().tracks.front().id);
        check(slots && slots->size() == 1, "the insert slot came back");
        check(slots && slots->front().uid == "com.daw.test.gain",
              "it still refers to the same plugin");
        check(slots &&
                  slots->front().channelMode == daw::PluginChannelMode::DualMono &&
                  slots->front().editorChannel ==
                      daw::PluginEditorChannel::Right &&
                  slots->front().sidechainTrackId == samplerTrackId,
              "wrapper mode, selected dual-mono side and sidechain survive reload");
        if (slots) {
            const auto wrapperParameters = reloaded.insertParameters(
                reloaded.project().tracks.front().id, slots->front().id);
            check(!wrapperParameters.empty() &&
                      std::fabs(reloaded.insertParameter(
                                    reloaded.project().tracks.front().id,
                                    slots->front().id,
                                    wrapperParameters.front().id) - 0.5) < 1e-6,
                  "the right dual-mono instance restores its own state chunk");
        }
        const daw::ClipModel* reloadedClip =
            reloaded.audioClip(reloaded.project().tracks.front().id, clipId);
        check(reloadedClip && reloadedClip->inserts.size() == 1 &&
                  reloadedClip->inserts.front().id == clipNestedId &&
                  reloaded.insertInstance(reloaded.project().tracks.front().id,
                                          clipNestedId) != nullptr,
              "clip FX slots and their live chain survive v4 reload");
        check(reloadedClip && std::fabs(reloadedClip->gain - 1.25f) < 1e-6f &&
                  std::fabs(reloadedClip->pan + 0.4f) < 1e-6f,
              "Clip strip Volume and Pan survive reload");
        check(std::fabs(reloaded.insertParameter(
                            reloaded.project().tracks.front().id, clipNestedId,
                            reloaded.insertParameters(
                                reloaded.project().tracks.front().id,
                                clipNestedId).front().id) - 0.25) < 1e-6,
              "clip plugin parameter state survives save and reload");
        const daw::TrackModel* reloadedSampler =
            reloaded.project().findTrack(samplerTrackId);
        check(reloadedSampler &&
                  reloadedSampler->samplerFx.ownerInstrumentId ==
                      reloadedSampler->instrument.id &&
                  reloadedSampler->samplerFx.inserts.size() == 8,
              "sampler FX ownership and all nested slots survive v3 reload");
        check(reloadedSampler &&
                  reloaded.trackNodes(samplerTrackId)->samplerInserts.size() == 8,
              "the reloaded nested chain is rebuilt in the graph");
        check(std::fabs(reloaded.insertParameter(
                            samplerTrackId, samplerNestedId,
                            reloaded.insertParameters(samplerTrackId,
                                                      samplerNestedId).front().id) -
                        0.42) < 1e-6,
              "nested plugin parameter state survives save and reload");

        const std::string afterPath = (dir / "after_reload.wav").string();
        check(reloaded.exportMixdown(afterPath, false).isOk(),
              "the reloaded project renders");
        check(std::fabs(peakOf(afterPath) - beforePeak) < 0.02f,
              "the plugin's saved state was restored — the mix sounds the same");
    }

    // ── A project written before plugin hosting still loads ──
    //
    // The v1 insert shape is three fields and nothing else. It must come back
    // as a free slot, keeping its name, rather than failing the load.
    {
        const fs::path legacy = dir / "Legacy.dawp";
        fs::create_directories(legacy, ec);
        std::ofstream(legacy / "project.json") << R"({
  "format": "daw-project",
  "version": 1,
  "name": "Legacy",
  "tempo": 120.0,
  "masterInserts": [{"id":"m1","name":"Old Master FX","bypassed":false}],
  "tracks": [{
    "id": "t1", "kind": "audio", "name": "Track",
    "inserts": [{"id":"i1","name":"Old Insert","bypassed":true}],
    "clips": []
  }]
})";

        daw::EngineController old;
        old.initialize(48000, 512, /*openDevice=*/false);
        check(old.openProject(legacy.string()).isOk(),
              "a v1 project without plugin fields loads");
        const std::vector<daw::InsertModel>* slots =
            old.channelInserts(old.project().tracks.front().id);
        check(slots && slots->size() == 1, "its insert slot survived");
        check(slots && !slots->front().isLoaded(),
              "the old placeholder reads as a free slot");
        check(slots && slots->front().name == "Old Insert" &&
                  slots->front().bypassed,
              "and keeps the name and bypass flag it was saved with");

        const fs::path v2 = dir / "Sampler-v2.dawp";
        fs::create_directories(v2, ec);
        std::ofstream(v2 / "project.json") << R"({
  "format": "daw-project", "version": 2, "name": "Sampler v2",
  "tempo": 120.0, "tracks": [{
    "id": "sampler-track", "kind": "instrument", "name": "Sampler",
    "instrument": {"id":"sampler-slot", "name":"Sampler",
      "format":"internal", "uid":"daw.sampler", "bypassed":false},
    "inserts": [], "clips": []
  }]
})";
        daw::EngineController oldSampler;
        oldSampler.initialize(48000, 512, /*openDevice=*/false);
        check(oldSampler.openProject(v2.string()).isOk(),
              "a v2 sampler project without samplerFx loads");
        const daw::TrackModel* migrated =
            oldSampler.project().findTrack("sampler-track");
        check(migrated && migrated->samplerFx.inserts.empty() &&
                  migrated->samplerFx.ownerInstrumentId == "sampler-slot",
              "the v2 sampler gains an empty owned strip without migration loss");
    }

    // ── A lane automates a plugin parameter ──
    //
    // The lane's values are 0…1 — that is what the piano roll draws and what
    // the model clamps them to — while the parameter is in its own plain units.
    // The mapping happens once, in the controller, against the range the live
    // plugin reports; storing plain values in the lane would make the drawn
    // curve meaningless the moment it was pointed at another parameter.
    {
        daw::EngineController automated;
        automated.initialize(48000, 512, /*openDevice=*/false);
        const std::string midiTrack =
            automated.addTrack(daw::TrackKind::Instrument, "Synth");

        // The fixture's instrument makes a steady tone to measure; the effect
        // after it is what gets automated.
        daw::plugins::PluginDescriptor instrument = found;
        instrument.uid = "com.daw.test.tone";
        instrument.isInstrument = true;
        daw::plugins::PluginDescriptor missingInstrument = instrument;
        missingInstrument.uid = "com.daw.missing.instrument";
        check(!automated.setTrackInstrumentPlugin(midiTrack, missingInstrument),
              "a VSTi/CLAP instrument that cannot instantiate is rejected");
        check(!automated.project().findTrack(midiTrack)->instrument.isLoaded(),
              "a failed instrument does not leave a silent phantom slot");
        check(automated.setTrackInstrumentPlugin(midiTrack, instrument),
              "a working instrument is accepted");
        const std::string slot = automated.addInsert(midiTrack, found);
        check(!slot.empty(), "an effect loads after the instrument");

        const std::string clip = automated.addMidiClip(midiTrack, 0.0, 4.0);
        daw::NoteModel note;
        note.id = "n";
        note.pitch = 64;
        note.startBeats = 0.0;
        note.lengthBeats = 8.0;   // 4 s at 120 BPM: the whole clip
        note.velocity = 127;
        automated.setClipNotes(midiTrack, clip, {note}, "Note");

        const auto parameters = automated.insertParameters(midiTrack, slot);
        check(!parameters.empty() && parameters[0].maxValue == 2.0,
              "the effect's gain runs 0 to 2");

        const std::string lane =
            automated.addControllerLane(midiTrack, clip, "Gain", -1);
        automated.setLaneTarget(midiTrack, clip, lane, slot, parameters[0].id);
        // A full-range ramp across the clip: 0 to 1 normalised is 0 to 2 plain.
        automated.setLanePoints(midiTrack, clip, lane, {{0.0, 0.0}, {8.0, 1.0}});

        const std::string rampPath = (dir / "ramp.wav").string();
        check(automated.exportMixdown(rampPath, false).isOk(),
              "exports with the lane driving the parameter");

        audio::platform::DecodedAudio decoded;
        check(audio::platform::decodeAudioFile(rampPath, decoded).isOk(),
              "the render decodes");
        auto sampleAt = [&](double seconds) {
            const std::size_t index =
                std::size_t(seconds * 48000.0) * std::size_t(decoded.channels);
            return index < decoded.interleaved.size()
                       ? std::fabs(decoded.interleaved[index])
                       : -1.0f;
        };
        // The instrument holds 1.0 while the note is on, so the output *is* the
        // gain: a straight line from 0 to 2 over the four seconds.
        const float rampQuarter = sampleAt(0.5);
        const float rampHalf = sampleAt(2.0);
        const float rampSevenEighths = sampleAt(3.5);
        check(std::fabs(rampQuarter - 0.25f) < 0.02f,
              "a quarter of the way in, the parameter is a quarter of the way up");
        check(std::fabs(rampHalf - 1.0f) < 0.02f, "halfway, halfway");
        check(std::fabs(rampSevenEighths - 1.75f) < 0.02f,
              "and seven eighths of the way in, seven eighths up");

        // Pointing the lane at nothing stops it driving the parameter — but it
        // does *not* rewind the parameter. Automation writes a value into the
        // plugin like any other edit; the plugin keeps the last one it was
        // given, which here is the 2.0 the ramp ended on. That is how every
        // host behaves, and the alternative — silently restoring some earlier
        // value when a lane is deleted — would be the surprising one.
        automated.setLaneTarget(midiTrack, clip, lane, slot, "");
        const std::string clearedPath = (dir / "cleared.wav").string();
        automated.exportMixdown(clearedPath, false);
        audio::platform::DecodedAudio cleared;
        audio::platform::decodeAudioFile(clearedPath, cleared);
        const std::size_t midpoint =
            std::size_t(2.0 * 48000.0) * std::size_t(cleared.channels);
        const float held = midpoint < cleared.interleaved.size()
                               ? std::fabs(cleared.interleaved[midpoint])
                               : -1.0f;
        check(std::fabs(held - 2.0f) < 0.02f,
              "and a lane pointed at nothing stops driving it, at the value it left");
    }

    // ── Routed audio bypasses sampler-owned FX, then hears track FX ──
    {
        daw::EngineController routed;
        routed.initialize(48000, 512, /*openDevice=*/false);
        const std::string source =
            routed.addTrack(daw::TrackKind::Audio, "Routed Source");
        routed.importAudio(tonePath, source, 0.0);
        const std::string target =
            routed.addTrack(daw::TrackKind::Instrument, "Sampler Target");
        const auto sampler = routed.pluginManager().find(
            daw::plugins::Format::Internal, "daw.sampler");
        routed.setTrackInstrumentPlugin(target, *sampler);
        const std::string owner =
            routed.project().findTrack(target)->instrument.id;
        check(routed.setTrackOutputBus(source, target),
              "audio can be routed into a sampler track");

        const std::string scoped =
            routed.addSamplerFxInsert(target, owner, found);
        const auto scopedParameters = routed.insertParameters(target, scoped);
        routed.setInsertParameter(target, scoped, scopedParameters.front().id, 0.25);
        const std::string scopedPath = (dir / "sampler-scope-bypass.wav").string();
        routed.exportMixdown(scopedPath, false);
        check(std::fabs(peakOf(scopedPath) - dryPeak) < 0.02f,
              "routed audio bypasses sampler-scoped effects");

        const std::string ordinary = routed.addInsert(target, found);
        const auto ordinaryParameters = routed.insertParameters(target, ordinary);
        routed.setInsertParameter(target, ordinary,
                                  ordinaryParameters.front().id, 0.5);
        const std::string ordinaryPath = (dir / "sampler-track-fx.wav").string();
        routed.exportMixdown(ordinaryPath, false);
        check(std::fabs(peakOf(ordinaryPath) - dryPeak * 0.5f) < 0.02f,
              "ordinary track effects remain the following independent stage");
    }

    // ── Solo silences every kind of track, not only audio ──
    // The bug: the fader's solo test read `kind == TrackKind::Audio`, so
    // soloing anything left every instrument and MIDI track playing. With a
    // synth in the project — which is most projects — solo did nothing at all.
    {
        daw::EngineController solo;
        solo.initialize(48000, 512, /*openDevice=*/false);
        const std::string audio = solo.addTrack(daw::TrackKind::Audio, "Audio");
        solo.importAudio(tonePath, audio, 0.0);

        daw::plugins::PluginDescriptor tone = found;
        tone.uid = "com.daw.test.tone";
        tone.isInstrument = true;
        const std::string synth = solo.addTrack(daw::TrackKind::Instrument, "Synth");
        check(solo.setTrackInstrumentPlugin(synth, tone),
              "the fixture instrument loads for the solo check");
        const std::string clip = solo.addMidiClip(synth, 0.0, 4.0);
        daw::NoteModel note;
        note.pitch = 60;
        note.startBeats = 0.0;
        note.lengthBeats = 8.0;
        note.velocity = 127;
        solo.setClipNotes(synth, clip, {note}, "Note");

        const std::string bothPath = (dir / "solo-both.wav").string();
        check(solo.exportMixdown(bothPath, false).isOk(), "exports both tracks");
        const float bothPeak = peakOf(bothPath);
        check(bothPeak > 0.9f, "the instrument's steady tone dominates the mix");

        // Solo the audio track: the synth has to go, and the tone stays.
        solo.setTrackSoloed(audio, true);
        const std::string soloedPath = (dir / "solo-audio.wav").string();
        check(solo.exportMixdown(soloedPath, false).isOk(), "exports with a solo");
        const float soloedPeak = peakOf(soloedPath);
        check(soloedPeak > 0.1f && soloedPeak < 0.6f,
              "soloing the audio track silences the instrument track");

        // And the other way round, which never worked either.
        solo.setTrackSoloed(audio, false);
        solo.setTrackSoloed(synth, true);
        const std::string synthPath = (dir / "solo-synth.wav").string();
        solo.exportMixdown(synthPath, false);
        const float synthPeak = peakOf(synthPath);
        check(synthPeak > 0.9f, "soloing the instrument keeps it audible");

        // A bus carrying a soloed track must stay open, or the solo silences
        // itself through its own destination.
        solo.setTrackSoloed(synth, false);
        const std::string bus = solo.addTrack(daw::TrackKind::Bus, "Bus");
        check(solo.setTrackOutputBus(audio, bus), "the audio track feeds a bus");
        solo.setTrackSoloed(audio, true);
        const std::string busPath = (dir / "solo-through-bus.wav").string();
        solo.exportMixdown(busPath, false);
        check(peakOf(busPath) > 0.1f,
              "a soloed track is still heard through the bus it feeds");

        solo.clearAllSolos();
        check(!solo.anySoloed(), "clearAllSolos lifts every solo");
        const std::string clearedPath = (dir / "solo-cleared.wav").string();
        solo.exportMixdown(clearedPath, false);
        check(std::fabs(peakOf(clearedPath) - bothPeak) < 0.02f,
              "and the mix comes back to what it was");
    }

    // ── A bus hears what is routed into it ──
    // The bug this pins down: a track's output and a send both used to be
    // connected straight to the destination's *fader*, which is past its
    // inserts. Signal arrived, the meter moved, and every plugin on the bus did
    // nothing at all.
    {
        daw::EngineController routed;
        routed.initialize(48000, 512, /*openDevice=*/false);
        const std::string src = routed.addTrack(daw::TrackKind::Audio, "Source");
        routed.importAudio(tonePath, src, 0.0);
        const std::string bus = routed.addTrack(daw::TrackKind::Bus, "Bus");
        check(routed.setTrackOutputBus(src, bus), "the source is routed to a bus");

        const std::string throughPath = (dir / "bus-dry.wav").string();
        check(routed.exportMixdown(throughPath, false).isOk(),
              "exports through the bus");
        const float throughPeak = peakOf(throughPath);
        check(throughPeak > 0.1f, "the bus passes the track to the master");

        const std::string slot = routed.addInsert(bus, found);
        check(!slot.empty(), "a plugin loads into the bus's insert slot");
        const auto parameters = routed.insertParameters(bus, slot);
        check(parameters.size() == 2, "the bus slot exposes the plugin's parameters");
        routed.setInsertParameter(bus, slot, parameters[0].id, 0.5);

        const std::string processedPath = (dir / "bus-half.wav").string();
        check(routed.exportMixdown(processedPath, false).isOk(),
              "exports with a plugin on the bus");
        check(std::fabs(peakOf(processedPath) - throughPeak * 0.5f) < 0.02f,
              "the bus's plugin processes what is routed into it");
    }

    // ── And so does a send ──
    // Pre-fader, with the track's own fader down, so the *only* path to the
    // master is send → bus → the bus's plugin.
    {
        daw::EngineController sent;
        sent.initialize(48000, 512, /*openDevice=*/false);
        const std::string src = sent.addTrack(daw::TrackKind::Audio, "Source");
        sent.importAudio(tonePath, src, 0.0);
        const std::string bus = sent.addTrack(daw::TrackKind::Bus, "Reverb");

        const std::string sendId = sent.addSend(src, bus);
        check(!sendId.empty(), "the send is created");
        sent.setSendPreFader(src, sendId, true);
        sent.setSendLevel(src, sendId, 1.0f);
        sent.setTrackVolume(src, 0.0f);

        const std::string wetPath = (dir / "send-dry.wav").string();
        check(sent.exportMixdown(wetPath, false).isOk(), "exports the send path");
        const float wetPeak = peakOf(wetPath);
        check(wetPeak > 0.1f, "the send alone reaches the master");

        const std::string slot = sent.addInsert(bus, found);
        const auto parameters = sent.insertParameters(bus, slot);
        check(parameters.size() == 2, "the send bus's slot exposes its parameters");
        sent.setInsertParameter(bus, slot, parameters[0].id, 0.5);

        const std::string processedPath = (dir / "send-half.wav").string();
        check(sent.exportMixdown(processedPath, false).isOk(),
              "exports the send through the bus's plugin");
        check(std::fabs(peakOf(processedPath) - wetPeak * 0.5f) < 0.02f,
              "a send is processed by the plugins on the bus it feeds");
    }

    // ── Copying a channel strip ──
    //
    // A chain is copied with the sound in it: the plugin's own state travels
    // with the slot, so the paste is the tuned plugin rather than a fresh one
    // wearing the same name.
    {
        daw::EngineController c;
        c.initialize(48000, 512, /*openDevice=*/false);
        c.pluginManager().setScannerPath(scannerPath);
        c.pluginManager().setSearchPaths(
            daw::plugins::Format::Clap,
            {fs::path(pluginPath).parent_path().string()});
        c.pluginManager().setSearchPaths(daw::plugins::Format::Vst3, {});
        c.pluginManager().setSearchPaths(daw::plugins::Format::AudioUnit, {});
        c.pluginManager().startScan();
        c.pluginManager().waitForScan();

        const std::string a = c.addTrack(daw::TrackKind::Audio, "A");
        const std::string b = c.addTrack(daw::TrackKind::Audio, "B");
        const std::string bus = c.addTrack(daw::TrackKind::Bus, "Bus");

        const std::string first = c.addInsert(a, found);
        const std::string second = c.addInsert(a, found);
        const auto parameters = c.insertParameters(a, first);
        check(!first.empty() && !second.empty() && parameters.size() == 2,
              "two plugins go onto the source channel");
        c.setInsertParameter(a, first, parameters[0].id, 0.25);
        c.setTrackVolume(a, 0.4f);
        c.setTrackPan(a, -0.5f);
        c.addSend(a, bus);

        // Plugins only.
        c.setChannelClipboard(c.copyChannelStrip(a, /*withSettings=*/false));
        check(c.channelClipboard().inserts.size() == 2,
              "the clipboard holds both slots");
        check(c.channelClipboard().sourceName == "A",
              "and remembers what it came from, for the paste menu");
        check(!c.channelClipboard().hasSettings,
              "a plugins-only copy takes no fader or sends with it");

        check(c.pasteChannelInserts(b, c.channelClipboard()),
              "the chain pastes onto another channel");
        const auto* pasted = c.channelInserts(b);
        check(pasted && pasted->size() == 2, "both plugins arrive");
        check(pasted && (*pasted)[0].id != first,
              "with slot ids of their own — a slot belongs to one chain");
        // The knob values travel with the slot. Read from the document rather
        // than from the plugin: a CLAP instance only learns a parameter changed
        // when it next processes a block, and nothing is rendering here — the
        // mixdown below is what proves the pasted chain really sounds like the
        // one it was copied from.
        check(pasted && pasted->front().parameters.size() == 1 &&
                  std::fabs(pasted->front().parameters.front().value - 0.25) < 1e-9,
              "and the plugin arrives holding the value it was copied at");
        check(std::fabs(c.project().findTrack(b)->volume - 1.0f) < 1e-6f &&
                  c.project().findTrack(b)->sends.empty(),
              "the rest of the target strip is untouched by a plugins-only paste");

        c.undo();
        check(c.channelInserts(b)->empty(), "undo takes the pasted chain away");
        c.redo();
        check(c.channelInserts(b)->size() == 2, "redo brings it back");

        // …and it is audible: a tone through the pasted chain comes out at the
        // gain the copied plugin was set to, not at the plugin's default.
        {
            c.importAudio(tonePath, b, 0.0);
            c.setTrackMuted(a, true);
            const std::string wet = (dir / "pasted-chain.wav").string();
            check(c.exportMixdown(wet, false).isOk(),
                  "exports through the pasted chain");
            check(std::fabs(peakOf(wet) - 0.5f * 0.25f) < 0.02f,
                  "and it is heard at the copied plugin's setting");
            c.setTrackMuted(a, false);
        }

        // The whole strip.
        c.setChannelClipboard(c.copyChannelStrip(a, /*withSettings=*/true));
        check(c.pasteChannelStrip(b, c.channelClipboard()),
              "the whole strip pastes");
        const daw::TrackModel* target = c.project().findTrack(b);
        check(target && std::fabs(target->volume - 0.4f) < 1e-6f &&
                  std::fabs(target->pan + 0.5f) < 1e-6f,
              "the fader and pan come with it");
        check(target && target->sends.size() == 1 &&
                  target->sends[0].destinationTrackId == bus,
              "and so do the sends — with ids of their own");
        check(target && target->sends[0].id != c.project().findTrack(a)->sends[0].id,
              "a pasted send is a new send, not the same one twice");
        c.undo();
        check(std::fabs(c.project().findTrack(b)->volume - 1.0f) < 1e-6f,
              "undoing a strip paste puts the fader back");

        // Dragging one plugin onto another channel.
        const std::size_t beforeA = c.channelInserts(a)->size();
        check(c.moveInsertBetweenChannels(a, second, bus, 0, /*copy=*/false),
              "a plugin moves to another channel");
        check(c.channelInserts(a)->size() == beforeA - 1 &&
                  c.channelInserts(bus)->size() == 1,
              "it leaves the channel it came from");
        c.undo();
        check(c.channelInserts(a)->size() == beforeA &&
                  c.channelInserts(bus)->empty(),
              "and one undo puts both channels back");

        check(c.moveInsertBetweenChannels(a, second, bus, 0, /*copy=*/true),
              "holding Alt copies it instead");
        check(c.channelInserts(a)->size() == beforeA &&
                  c.channelInserts(bus)->size() == 1,
              "so the source keeps its own");

        // Dragging the sends across.
        const std::string d = c.addTrack(daw::TrackKind::Audio, "D");
        check(c.copySendsTo(a, d, /*move=*/false),
              "sends copy onto another channel");
        check(c.project().findTrack(d)->sends.size() == 1 &&
                  c.project().findTrack(a)->sends.size() == 1,
              "a copy leaves the source's sends alone");
        check(c.copySendsTo(a, d, /*move=*/true),
              "and a plain drag moves them");
        check(c.project().findTrack(a)->sends.empty(),
              "so the source is left with none");
    }

    fs::remove_all(dir, ec);
    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
