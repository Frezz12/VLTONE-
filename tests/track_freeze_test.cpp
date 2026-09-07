#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Internal/InternalFactory.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "platform/AudioFileDecoder.hpp"
#include <filesystem>
#include <cstdio>
#include <cmath>
namespace fs = std::filesystem;
static int failures = 0;
static bool check(bool ok, const char* text) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", text); failures += !ok; return ok; }
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto temp = fs::temp_directory_path() / ("vlt-freeze-test-" + daw::newUuid());
    fs::create_directories(temp);
    audio::AudioBuffer input(2, 12000);
    for (unsigned i = 0; i < 12000; ++i) for (unsigned c = 0; c < 2; ++c)
        input.getChannel(c)[i] = .1f * std::sin(float(i) * .1f);
    audio::AudioRecorder recorder; recorder.initialize(48000, 2);
    recorder.writeWAVFile((temp / "source.wav").string(), input, 48000);
    {
        daw::EngineController controller;
        if (!check(bool(controller.initialize(48000, 256, false)), "headless freeze controller initializes")) return 1;
        controller.setRecordDirectory(temp.string());
        auto id = controller.importAudioToNewTrack((temp / "source.wav").string(), 0.0);
        if (!check(!id.empty(), "freeze source imports")) return 1;
        std::string eqSlot;
        for (const auto& plugin : daw::plugins::builtinPlugins()) if (plugin.uid == "daw.equalizer")
            eqSlot = controller.addInsert(id, plugin);
        check(!eqSlot.empty(), "freeze source includes a real plugin insert");
        const auto bus = controller.addTrack(daw::TrackKind::Bus, "Downstream bus");
        for (const auto& plugin : daw::plugins::builtinPlugins()) if (plugin.uid == "daw.equalizer") {
            const auto slot = controller.addInsert(bus, plugin);
            controller.setInsertParameter(bus, slot, "processing.mode", 2.0);
        }
        controller.setTrackOutputBus(id, bus);
        controller.setInsertParameter(id, eqSlot, "processing.mode", 2.0);
        controller.pumpPluginEvents();
        const auto muted = controller.duplicateClipAt(id, controller.project().findTrack(id)->clips.front().id, 0.0);
        controller.setClipMuted(id, muted, true);
        const auto clips = controller.project().findTrack(id)->clips.size();
        controller.setTrackVolume(id, .5f);
        auto render = [&](const char* name) {
            daw::rendering::Spec spec; spec.outputDir = temp.string(); spec.baseName = name;
            spec.file.container = audio::platform::Container::Wav; spec.file.encoding = audio::platform::Encoding::Float32;
            spec.range = daw::rendering::Range::Custom; spec.customEndSeconds = .25;
            daw::rendering::Report report;
            const auto result = controller.renderProject(spec, {}, report);
            check(bool(result) && report.files.size() == 1, "freeze comparison render completes");
            audio::platform::DecodedAudio decoded;
            if (!report.files.empty()) (void)audio::platform::decodeAudioFile(report.files.front(), decoded);
            return decoded.interleaved;
        };
        const auto before = render("before");
        const auto originalNodes = controller.routingGraph()->nodes.size();
        daw::rendering::Report frozen;
        auto result = controller.freezeTrack(id, {}, frozen);
        if (!result) std::printf("freeze error: %s\n", result.message().c_str());
        check(bool(result) && !frozen.cancelled && controller.isTrackFrozen(id), "explicit freeze publishes current render");
        check(controller.project().findTrack(id)->clips.size() == clips, "freeze preserves original editable clips");
        check(controller.routingGraph()->nodes.size() < originalNodes, "freeze removes original DSP nodes from the scheduled graph");
        auto after = render("after");
        float error = 0;
        if (before.size() == after.size()) for (std::size_t i = 0; i < before.size(); ++i) error = std::max(error, std::abs(before[i] - after[i]));
        check(!before.empty() && before.size() == after.size() && error < 1e-6f, "frozen pre-fader playback matches original render");
        controller.setTrackVolume(id, .25f); controller.setTrackPan(id, -.25f);
        check(controller.isTrackFrozen(id), "fader and pan remain editable without thawing");
        const auto package = (temp / "session.vlt").string();
        check(bool(controller.saveProject(package)), "freeze and original sources save as portable project");
        {
            daw::EngineController reopened; reopened.initialize(48000, 256, false);
            check(bool(reopened.openProject(package)) && reopened.isTrackFrozen(id), "frozen project reopens with source state intact");
            reopened.unfreezeTrack(id);
            check(!reopened.isTrackFrozen(id) && reopened.project().findTrack(id)->clips.size() == clips, "unfreeze restores editable source");
            reopened.undo(); check(reopened.isTrackFrozen(id), "unfreeze is undoable");
            reopened.redo(); check(!reopened.isTrackFrozen(id), "unfreeze is redoable");
            reopened.shutdown();
        }
        controller.unfreezeTrack(id);
        daw::rendering::Report cancelled;
        result = controller.freezeTrack(id, [](const auto&) { return false; }, cancelled);
        check(bool(result) && cancelled.cancelled && !controller.isTrackFrozen(id) && cancelled.files.empty(), "cancelled freeze never publishes a partial result");
        bool changed = false;
        result = controller.freezeTrack(id, [&](const auto&) { if (!changed) { changed = true; controller.setTrackVolume(id, .4f); } return true; }, cancelled);
        check(bool(result) && cancelled.cancelled && !controller.isTrackFrozen(id), "revision change rejects stale freeze publication");
        result = controller.freezeTrack(id, {}, frozen);
        check(bool(result) && controller.isTrackFrozen(id), "plugin track can be frozen again");
        controller.setInsertParameter(id, eqSlot, "output.gain", -3.0);
        check(!controller.isTrackFrozen(id), "editing source DSP thaws the original chain before queuing parameters");
        controller.setTrackArmed(id, true);
        check(!controller.freezeUnavailableReason(id).empty(), "recording tracks reject independent freeze");
        controller.setTrackArmed(id, false);
        controller.shutdown();
        for (const auto& file : frozen.files) { std::error_code ec; fs::remove(file, ec); }
    }
    std::error_code ec; fs::remove_all(temp, ec);
    return failures ? 1 : 0;
}
