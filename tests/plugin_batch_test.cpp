#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "Recording/RecordingEngine.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>

using Controller = daw::EngineController;
using Target = Controller::PluginBatchTarget;
namespace fs = std::filesystem;
static int failures = 0;
static bool check(bool value, const char* message) {
    std::printf("%s  %s\n", value ? "PASS" : "FAIL", message);
    if (!value) ++failures;
    return value;
}
static std::string document(const Controller& controller) {
    std::string out; daw::ProjectSerializer::serializeDocument(controller.project(), out); return out;
}
static double level(Controller& controller) {
    audio::AudioBuffer input(2, 256), output(2, 256);
    double energy = 0;
    for (int block = 0; block < 100; ++block) {
        controller.processDeviceBlockForTest(input, output, 256);
        if (block < 80) continue;
        for (unsigned i = 0; i < 256; ++i) energy += double(output.getChannel(0)[i]) * output.getChannel(0)[i];
    }
    return std::sqrt(energy / (20 * 256));
}
static double gain(const Controller& c, const std::string& track, const std::string& id) {
    const auto* model = c.insertModel(track, id);
    if (model) for (const auto& parameter : model->parameters)
        if (parameter.id == "output.gain") return parameter.value;
    return 999;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir = fs::temp_directory_path() / "vlt_plugin_batch_test";
    fs::create_directories(dir);
    const auto wav = (dir / "source.wav").string();
    audio::AudioBuffer tone(2, 48000);
    for (unsigned i = 0; i < 48000; ++i) {
        const float sample = .25f * std::sin(float(i * 2.0 * 3.141592653589793 * 440.0 / 48000.0));
        tone.getChannel(0)[i] = tone.getChannel(1)[i] = sample;
    }
    audio::AudioRecorder recorder; recorder.initialize(48000, 2);
    recorder.writeWAVFile(wav, tone, 48000);
    Controller c;
    check(c.initialize(48000, 256, false).isOk(), "headless audio initializes");
    const auto lead = c.addTrack(daw::TrackKind::Audio, "Lead");
    const auto doubleTrack = c.addTrack(daw::TrackKind::Audio, "Double");
    const auto midi = c.addTrack(daw::TrackKind::Midi, "MIDI");
    const auto automation = c.addTrack(daw::TrackKind::Automation, "Automation");
    const auto leadClip = c.importAudio(wav, lead, 0.0);
    const auto doubleClip = c.importAudio(wav, doubleTrack, 0.0);
    const auto& eq = daw::plugins::equalizer::EqualizerInstance::staticDescriptor();
    const auto oldFx = c.addInsert(doubleTrack, eq);
    c.setInsertParameter(doubleTrack, oldFx, "output.gain", -3.0);
    const std::vector<Target> tracks{{lead, {}}, {doubleTrack, {}}, {midi, {}}};
    const std::vector<Target> clips{{lead, leadClip}, {doubleTrack, doubleClip}};
    check(c.validatePluginBatch(tracks, 1).isOk(), "audio and MIDI tracks accept the same insert batch");
    check(!c.validatePluginBatch({{lead, {}}, {automation, {}}}), "automation track rejected");
    check(!c.validatePluginBatch({{lead, {}}, {doubleTrack, doubleClip}}), "track Inserts and Clip FX cannot mix");
    check(!c.validatePluginBatch({{lead, {}}, {lead, {}}}), "duplicate target rejected");

    const auto before = document(c);
    const auto depth = c.undoDepth();
    std::shared_ptr<Controller> draft;
    if (!check(c.createPluginBatchDraft({lead, {}}, draft).isOk(), "isolated source draft created")) return 1;
    const auto fx = draft->addInsert(lead, eq);
    draft->setInsertParameter(lead, fx, "output.gain", -6.020599913);
    draft->pumpPreviewPluginEvents();
    Controller::ChannelSnapshot settings;
    check(draft->capturePluginBatchChain({lead, {}}, {fx}, settings).isOk(), "captures vendor settings for just the added effect");
    check(!settings.inserts.empty() && !settings.inserts.front().state.empty(), "opaque plugin state retained");
    check(c.startPluginAudition(draft).isOk(), "audition is driven by the existing audio device callback");
    const double quiet = level(c);
    draft->setInsertParameter(lead, fx, "output.gain", 0.0);
    const double loud = level(c);
    check(quiet > .01 && std::abs(loud / quiet - 2.0) < .03, "live parameter adjustment changes audible output on the selected source");
    c.stopPluginAudition();
    check(level(c) < 1.e-7, "stopping audition removes its signal from the device output");
    check(document(c) == before && c.undoDepth() == depth, "audition and cancel leave project and undo history unchanged");

    std::vector<std::vector<std::string>> ids;
    check(c.appendPluginBatch(tracks, settings, ids).isOk() && ids.size() == 3, "applies settings to every selected track");
    if (ids.size() != 3) return 1;
    check(c.project().findTrack(doubleTrack)->inserts.front().id == oldFx &&
        c.project().findTrack(doubleTrack)->inserts.size() == 2, "existing effects preserved and new effects appended");
    check(ids[0][0] != ids[1][0] && c.insertInstance(lead, ids[0][0]) != c.insertInstance(doubleTrack, ids[1][0]), "each destination owns an independent plugin instance");
    for (std::size_t i = 0; i < tracks.size(); ++i)
        check(std::abs(gain(c, tracks[i].trackId, ids[i][0]) + 6.020599913) < .01, "configured gain copied to destination");
    check(c.undoDepth() == depth + 1, "batch creates exactly one undo operation");
    c.undo(); check(document(c) == before, "one undo restores every target exactly");
    c.redo(); check(c.project().findTrack(doubleTrack)->inserts.size() == 2 &&
        std::abs(gain(c, lead, ids[0][0]) + 6.020599913) < .01, "redo restores settings, not default plugins");
    c.setInsertParameter(lead, ids[0][0], "output.gain", 2.0);
    check(std::abs(gain(c, doubleTrack, ids[1][0]) + 6.020599913) < .01, "editing one applied copy does not change other copies");

    std::shared_ptr<Controller> secondSource;
    check(c.createPluginBatchDraft({doubleTrack, {}}, secondSource).isOk(), "can choose another source with existing effects");
    std::vector<std::vector<std::string>> secondIds;
    check(secondSource->appendPluginBatch({{doubleTrack, {}}}, settings, secondIds).isOk(), "draft chain moves to another source");
    check(secondSource->project().findTrack(doubleTrack)->inserts.size() == 3 &&
        std::abs(gain(*secondSource, doubleTrack, secondIds.front().front()) + 6.020599913) < .01,
        "source switch retains settings alongside the new source's original effects");

    const auto beforeClips = document(c);
    check(c.appendPluginBatch(clips, settings, ids).isOk() && c.clipFx(lead, leadClip)->size() == 1 &&
        c.clipFx(doubleTrack, doubleClip)->size() == 1, "audio clip batch populates Clip FX on every clip");
    c.undo(); check(document(c) == beforeClips, "Clip FX batch undo leaves track Inserts intact");
    c.redo();
    const auto saved = (dir / "Batch.vlt").string();
    check(c.saveProject(saved).isOk(), "configured track and clip effects save to project");
    Controller reopened; reopened.initialize(48000, 256, false);
    check(reopened.openProject(saved).isOk() && reopened.clipFx(lead, leadClip)->size() == 1 &&
        std::abs(gain(reopened, lead, ids[0][0]) + 6.020599913) < .01, "project reload preserves Clip FX settings");
    const auto beforeFailure = document(c);
    const auto failureDepth = c.undoDepth();
    auto invalid = settings; invalid.inserts.front().state = {0xde, 0xad};
    check(!c.appendPluginBatch(tracks, invalid, ids) && ids.empty() && document(c) == beforeFailure &&
        c.undoDepth() == failureDepth, "plugin state load failure rolls the whole batch back without an undo entry");
    for (int i = 1; i < 8; ++i) c.addClipFxInsert(lead, leadClip, eq);
    const auto full = document(c);
    check(!c.appendPluginBatch(clips, settings, ids) && document(c) == full,
        "a full Clip FX rack prevents partial application to other clips");
    const auto midiClip = c.addMidiClip(midi, 0.0);
    check(!c.validatePluginBatch({{lead, leadClip}, {midi, midiClip}}), "MIDI clips cannot be mixed into an audio Clip FX batch");
    std::shared_ptr<Controller> clipDraft;
    check(c.createPluginBatchDraft({doubleTrack, doubleClip}, clipDraft).isOk(), "audio clip can be auditioned with its existing Clip FX and inserts");
    check(c.startPluginAudition(clipDraft).isOk() && level(c) > .001, "clip audition produces audio");
    c.stopPluginAudition();
    const auto bus = c.addTrack(daw::TrackKind::Bus, "Vocal bus");
    c.setTrackOutputBus(doubleTrack, bus);
    std::shared_ptr<Controller> busDraft;
    check(c.createPluginBatchDraft({bus, {}}, busDraft).isOk(), "bus source includes its upstream tracks");
    check(c.startPluginAudition(busDraft).isOk() && level(c) > .001, "bus audition produces routed audio");
    c.stopPluginAudition();
    check(c.loadInstrumentSampler(midi, wav), "MIDI source instrument loaded");
    c.addNote(midi, midiClip, 60, 0.0, 4.0);
    std::shared_ptr<Controller> midiDraft;
    check(c.createPluginBatchDraft({midi, {}}, midiDraft).isOk(), "MIDI source restores its instrument state");
    check(c.startPluginAudition(midiDraft).isOk() && level(c) > .001, "MIDI source audition produces instrument audio");
    c.stopPluginAudition();
    daw::TrackModel unavailable;
    unavailable.id = daw::newUuid(); unavailable.kind = daw::TrackKind::Audio;
    unavailable.name = "Unrelated missing plugin";
    daw::InsertModel missing; missing.id = daw::newUuid(); missing.name = "Missing effect";
    missing.format = daw::PluginFormat::Clap; missing.uid = "test.unavailable.batch-effect";
    unavailable.inserts.push_back(missing);
    auto fixture = c.prepareProjectSave();
    fixture.project.tracks.push_back(unavailable);
    const auto missingPackage = (dir / "Missing.vlt").string();
    check(Controller::writePreparedProject(fixture, missingPackage).isOk() && c.openProject(missingPackage).isOk(),
        "opens a project fixture containing an unavailable unrelated effect");
    check(c.createPluginBatchDraft({doubleTrack, {}}, secondSource).isOk() &&
        secondSource->project().findTrack(unavailable.id)->inserts.empty(),
        "unrelated missing plugins do not block or run during audition");
    fs::remove_all(dir);
    return failures ? 1 : 0;
}
