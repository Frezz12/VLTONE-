#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "platform/AudioFileDecoder.hpp"
#include "cloud/CloudDocumentProjection.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;

static bool check(bool condition, const char* message) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition) ++failures;
    return condition;
}

static void writeTone(const std::string& path) {
    constexpr double rate = 48000.0;
    constexpr audio::BufferSize frames = 12000;
    audio::AudioBuffer tone(2, frames);
    for (audio::BufferSize frame = 0; frame < frames; ++frame) {
        const float sample = 0.35f * std::sin(
            float(2.0 * 3.141592653589793 * 440.0 * double(frame) / rate));
        tone.getChannel(0)[frame] = sample;
        tone.getChannel(1)[frame] = sample;
    }
    audio::AudioRecorder recorder;
    recorder.initialize(rate, 2);
    recorder.writeWAVFile(path, tone, rate);
}

static double rmsFile(const std::string& path) {
    audio::platform::DecodedAudio audio;
    if (!check(audio::platform::decodeAudioFile(path, audio).isOk(), "rendered audio decodes")) return 0.0;
    double energy = 0.0;
    for (float sample : audio.interleaved) energy += double(sample) * sample;
    return audio.interleaved.empty() ? 0.0 : std::sqrt(energy / audio.interleaved.size());
}

static const daw::ClipModel* findClip(const daw::EngineController& controller,
                                      const std::string& trackId,
                                      const std::string& clipId) {
    const daw::TrackModel* track = controller.project().findTrack(trackId);
    if (!track) return nullptr;
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [&](const daw::ClipModel& clip) { return clip.id == clipId; });
    return found == track->clips.end() ? nullptr : &*found;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "daw_processing_render_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string tone = (dir / "tone.wav").string();
    writeTone(tone);

    // Partial Replace is staged first, splits only the covered middle, and is
    // one undo/redo operation. With no FX layer printed, the bounce re-enters
    // at TrackSource so it cannot traverse a printed stage twice.
    {
        daw::EngineController controller;
        check(controller.initialize(48000, 256, false).isOk(),
              "bounce controller initializes headless");
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Source");
        const std::string clip = controller.importAudio(tone, track, 0.0);
        daw::EngineController::BounceRequest request;
        request.clips = {{track, clip}};
        request.startSeconds = 0.05;
        request.endSeconds = 0.15;
        request.fxLayers = 0;
        request.destination = daw::EngineController::BounceDestination::Replace;
        daw::EngineController::BounceReport report;
        const audio::Result result =
            controller.bounceInPlace(request, {}, report);
        check(result.isOk() && report.outputs.size() == 1,
              "partial Bounce in Place succeeds");
        const daw::TrackModel* after = controller.project().findTrack(track);
        check(after && after->clips.size() == 3,
              "partial Replace keeps the two outside pieces");
        const daw::ClipModel* bounced =
            report.outputs.empty()
                ? nullptr
                : findClip(controller, track, report.outputs.front().clipId);
        check(bounced && bounced->playbackInjection.stage ==
                             daw::PlaybackInjectionStage::TrackSource,
              "dry bounce injects at track source");
        check(!report.outputs.empty() &&
                  fs::exists(report.outputs.front().filePath),
              "bounce output is a managed float WAV");
        controller.undo();
        after = controller.project().findTrack(track);
        check(after && after->clips.size() == 1 &&
                  after->clips.front().id == clip,
              "Bounce in Place undoes as one transaction");
        controller.redo();
        after = controller.project().findTrack(track);
        check(after && after->clips.size() == 3,
              "Bounce in Place redo restores generated clips");

        const std::size_t beforeCancel = after ? after->clips.size() : 0;
        request.startSeconds = 0.0;
        request.endSeconds = 0.2;
        request.clips = {{track, after->clips.front().id}};
        daw::EngineController::BounceReport cancelled;
        const audio::Result cancelledResult = controller.bounceInPlace(
            request,
            [](const daw::rendering::Progress&) { return false; }, cancelled);
        after = controller.project().findTrack(track);
        check(cancelledResult.isOk() && cancelled.cancelled && after &&
                  after->clips.size() == beforeCancel,
              "cancelled bounce leaves the project unchanged");
    }

    // MIDI/Pattern material cannot be replaced by an audio clip on its source
    // lane. The covered source segment is muted and the bounce lands on the
    // adjacent audio lane instead.
    {
        daw::EngineController controller;
        controller.initialize(48000, 256, false);
        const std::string track =
            controller.addTrack(daw::TrackKind::Midi, "MIDI Source");
        const std::string clip = controller.addMidiClip(track, 0.0, 0.25);
        daw::EngineController::BounceRequest request;
        request.clips = {{track, clip}};
        request.startSeconds = 0.05;
        request.endSeconds = 0.15;
        request.destination = daw::EngineController::BounceDestination::Replace;
        daw::EngineController::BounceReport report;
        check(controller.bounceInPlace(request, {}, report).isOk() &&
                  report.outputs.size() == 1,
              "MIDI Replace bounces to an adjacent audio track");
        const daw::TrackModel* source = controller.project().findTrack(track);
        const daw::TrackModel* destination =
            report.outputs.empty()
                ? nullptr
                : controller.project().findTrack(
                      report.outputs.front().destinationTrackId);
        check(source && source->clips.size() == 3 &&
                  std::count_if(source->clips.begin(), source->clips.end(),
                                [](const daw::ClipModel& item) {
                                    return item.muted;
                                }) == 1,
              "MIDI Replace mutes only the covered middle segment");
        check(destination && destination->kind == daw::TrackKind::Audio &&
                  controller.project().indexOf(destination->id) ==
                      controller.project().indexOf(track) + 1,
              "non-audio bounce destination is immediately after its source");
    }

    // A bounce placed on a new audio track must follow that track's channel
    // strip. The old semantic injection anchored it to the source track, so
    // moving the clip looked right while the source fader/mute still owned it.
    {
        daw::EngineController controller;
        controller.initialize(48000, 256, false);
        const std::string source =
            controller.addTrack(daw::TrackKind::Audio, "Route Source");
        controller.setTrackVolume(source, 0.4f);
        controller.setTrackPan(source, -0.25f);
        const std::string clip = controller.importAudio(tone, source, 0.0);
        daw::EngineController::BounceRequest request;
        request.clips = {{source, clip}};
        request.startSeconds = 0.0;
        request.endSeconds = 0.2;
        request.destination =
            daw::EngineController::BounceDestination::NewTrack;
        daw::EngineController::BounceReport report;
        check(controller.bounceInPlace(request, {}, report).isOk() &&
                  report.outputs.size() == 1,
              "Bounce in Place creates a new audio track");
        const daw::ClipModel* bounced =
            report.outputs.empty()
                ? nullptr
                : findClip(controller,
                           report.outputs.front().destinationTrackId,
                           report.outputs.front().clipId);
        check(bounced && !bounced->playbackInjection.active(),
              "new-track bounce follows its visible channel strip");
        const daw::TrackModel* destination =
            report.outputs.empty()
                ? nullptr
                : controller.project().findTrack(
                      report.outputs.front().destinationTrackId);
        check(destination && std::abs(destination->volume - 0.4f) < 1e-6f &&
                  std::abs(destination->pan + 0.25f) < 1e-6f,
              "new-track bounce moves the still-live fader and pan");
    }
    // Layered offline processing uses the currently heard audio, with portable
    // immutable history and no retained live chain.
    {
        daw::EngineController controller;
        check(controller.initialize(48000, 256, false).isOk(), "offline controller initializes headless");
        const auto track = controller.addTrack(daw::TrackKind::Audio, "Offline Source");
        const auto clip = controller.importAudio(tone, track, 0.4);
        const auto other = controller.importAudio(tone, track, 0.8);
        const daw::EngineController::ClipAddress address{track, clip};
        const daw::EngineController::ClipAddress second{track, other};
        controller.setClipGain(track, clip, 0.5f);
        const auto liveFx = controller.addClipFxInsert(track, clip,
            daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
        controller.setInsertParameter(track, liveFx, "output.gain", 12.0);

        daw::EngineController rack;
        rack.initialize(48000, 256, false);
        const auto rackTrack = rack.addTrack(daw::TrackKind::Audio, "Rack");
        const auto equalizer = rack.addInsert(rackTrack,
            daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
        rack.setInsertParameter(rackTrack, equalizer, "output.gain", -6.020599913);
        auto chain = rack.copyChannelStrip(rackTrack, false);
        daw::EngineController::OfflineRenderReport rendered;
        check(controller.renderClipsOffline({address, second, address}, chain, false, {}, rendered).isOk() &&
              rendered.files.size() == 2, "multi-clip render replaces each selected clip once");
        const auto* processed = findClip(controller, track, clip);
        check(processed && processed->offlineHistory.size() == 2 && processed->offlineProcess.empty() &&
              controller.offlineProcessChain(address).inserts.empty(),
              "original and first render persist without reopening the previous rack");
        const std::string firstFile = processed->filePath;
        const std::string firstVersion = processed->offlineVersionId;
        const std::string originalVersion = processed->offlineHistory.front().id;
        check(processed->gain == 1.0f && processed->inserts.front().id == liveFx &&
              std::abs(processed->startSeconds - 0.4) < 1e-6,
              "render resets printed gain, retains realtime FX, identity and timeline position");
        check(std::abs(rmsFile(firstFile) / rmsFile(tone) - 0.25) < 0.015,
              "clip gain and offline plugin print exactly once; live clip FX do not print");

        controller.undo();
        check(findClip(controller, track, clip)->filePath == tone &&
              findClip(controller, track, other)->filePath == tone,
              "one undo restores the entire batch");
        controller.redo();
        check(findClip(controller, track, clip)->filePath == firstFile &&
              findClip(controller, track, other)->offlineHistory.size() == 2,
              "one redo restores the entire batch and history");
        daw::EngineController::OfflineRenderReport layered;
        check(controller.renderClipsOffline({address}, chain, false, {}, layered).isOk(),
              "a second render adds a processing layer");
        processed = findClip(controller, track, clip);
        const auto secondVersion = processed->offlineVersionId;
        const auto secondFile = processed->filePath;
        check(processed->offlineHistory.size() == 3 &&
              processed->offlineHistory.back().parentId == firstVersion &&
              std::abs(rmsFile(secondFile) / rmsFile(firstFile) - 0.5) < 0.015,
              "second layer processes the first render rather than the original");
        controller.setClipGain(track, clip, 0.7f);
        check(controller.clipPlaybackFilePath(*findClip(controller, track, clip)) == secondFile,
              "gain changes never discard offline processing");
        check(controller.selectOfflineRenderVersion(address, firstVersion).isOk() &&
              findClip(controller, track, clip)->filePath == firstFile,
              "history selection switches playback to an earlier render");
        check(controller.renderClipsOffline({address}, chain, false, {}, layered).isOk(),
              "rendering an earlier version appends a new version");
        processed = findClip(controller, track, clip);
        check(processed->offlineHistory.size() == 4 &&
              processed->offlineHistory.back().parentId == firstVersion &&
              processed->offlineHistory[2].id == secondVersion,
              "history retains later versions when rendering from an earlier one");
        check(controller.restoreOfflineRenderOriginal(address).isOk(), "restore original succeeds");
        processed = findClip(controller, track, clip);
        check(processed->filePath == tone && processed->gain == 0.5f &&
              processed->offlineVersionId == originalVersion && processed->offlineHistory.size() == 4 &&
              processed->inserts.front().id == liveFx, "restore recovers the first audio state and keeps history/live FX");
        controller.undo();
        check(findClip(controller, track, clip)->offlineVersionId != originalVersion,
              "history switches and restore-original are undoable");
        controller.redo();
        controller.selectOfflineRenderVersion(address, secondVersion);
        const auto recovery = controller.captureRecoverySnapshot();
        check(recovery.project.findTrack(track)->clips.front().offlineHistory.size() == 4,
              "crash recovery retains every offline version");

        const auto package = (dir / "processed.vlt").string();
        check(controller.saveProject(package).isOk(), "project package saves offline history");
        daw::EngineController reopened;
        reopened.initialize(48000, 256, false);
        check(reopened.openProject(package).isOk(), "project package reopens offline history");
        const auto* reloaded = findClip(reopened, track, clip);
        check(reloaded && reloaded->offlineHistory.size() == 4 && reloaded->offlineVersionId == secondVersion,
              "active version and history survive reopen");
        bool portable = true;
        for (const auto& version : reloaded->offlineHistory)
            portable &= fs::exists(version.source.filePath) && version.source.filePath.find("Content") != std::string::npos;
        check(portable, "all versions including the original are copied into package Content");
        check(reopened.restoreOfflineRenderOriginal(address).isOk() &&
              reopened.selectOfflineRenderVersion(address, secondVersion).isOk(),
              "original and processed versions remain selectable after reopen");
        check(!reopened.exportMixdown(reloaded->offlineHistory.front().source.filePath),
              "mixdown cannot overwrite an inactive original history file");

        const auto durationBefore = findClip(controller, track, clip)->durationSeconds;
        daw::EngineController::OfflineRenderReport tailed;
        check(controller.renderClipsOffline({address}, chain, true, {}, tailed).isOk(),
              "offline processing can include a silence-detected tail");
        processed = findClip(controller, track, clip);
        check(processed->durationSeconds > durationBefore &&
              controller.clipPlaybackDuration(*processed) == processed->durationSeconds,
              "tail extends the actual visible and playable source duration");
        const auto tailDuration = processed->durationSeconds;
        check(controller.renderClipsOffline({address}, chain, false, {}, layered).isOk() &&
              std::abs(controller.audioClip(track, clip)->durationSeconds - tailDuration) < 1e-5,
              "the next layer includes the full previous effect tail");
        processed = findClip(controller, track, clip);
        const auto historySize = processed->offlineHistory.size();
        const auto currentFile = processed->filePath;
        chain.inserts.front().model.bypassed = true;
        check(!controller.renderClipsOffline({address}, chain, false, {}, rendered) &&
              !controller.renderClipsOffline({address}, {}, false, {}, rendered),
              "empty and fully bypassed chains cannot silently restore original audio");
        check(findClip(controller, track, clip)->filePath == currentFile &&
              findClip(controller, track, clip)->offlineHistory.size() == historySize,
              "a no-effect render leaves playback and history untouched");
        auto unavailable = chain;
        unavailable.inserts.front().model.bypassed = false;
        unavailable.inserts.front().model.format = daw::PluginFormat::Vst3;
        unavailable.inserts.front().model.uid = "missing.test.plugin";
        unavailable.inserts.front().model.name = "Missing Test Plugin";
        check(!controller.renderClipsOffline({address}, unavailable, false, {}, rendered) &&
              findClip(controller, track, clip)->filePath == currentFile,
              "an unavailable enabled plugin fails atomically");
        check(!controller.selectOfflineRenderVersion(address, "unknown") &&
              findClip(controller, track, clip)->filePath == currentFile,
              "invalid history selection leaves playback untouched");
        rack.shutdown();
    }
    // file and leaves neither clip with a partial chain/cache mutation.
    {
        daw::EngineController controller;
        controller.initialize(48000, 256, false);
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Atomic Offline");
        const auto renderDir = dir / "atomic";
        fs::create_directories(renderDir);
        controller.setRecordDirectory(renderDir.string());
        const std::string first = controller.importAudio(tone, track, 0.0);
        const std::string second = controller.importAudio(tone, track, 0.3);

        daw::EngineController rack;
        rack.initialize(48000, 256, false);
        const std::string rackTrack =
            rack.addTrack(daw::TrackKind::Audio, "Rack");
        rack.addInsert(
            rackTrack,
            daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
        const auto chain = rack.copyChannelStrip(rackTrack, false);
        daw::EngineController::OfflineRenderReport report;
        const audio::Result cancelled = controller.renderClipsOffline(
            {{track, first}, {track, second}}, chain, false,
            [](const daw::rendering::Progress& progress) {
                return progress.fraction < 0.6;
            },
            report);
        const daw::ClipModel* firstAfter = findClip(controller, track, first);
        const daw::ClipModel* secondAfter = findClip(controller, track, second);
        check(cancelled.isOk() && report.cancelled,
              "multi-clip Offline Render can cancel during a later clip");
        check(firstAfter && secondAfter && firstAfter->offlineHistory.empty() &&
                  secondAfter->offlineHistory.empty() && fs::is_empty(renderDir),
              "later cancellation rolls back the complete offline batch");
        check(!controller.offlineRenderInProgress(), "cancellation releases the controller");
        const auto failed = controller.renderClipsOffline({{track, first}, {track, second}}, chain, false,
            [](const auto&) -> bool { throw std::runtime_error("progress failed"); }, report);
        check(!failed && fs::is_empty(renderDir) && !controller.offlineRenderInProgress(),
              "progress exceptions clean up every file and release the controller");
        bool changed = false;
        const auto stale = controller.renderClipsOffline({{track, first}, {track, second}}, chain, false,
            [&](const auto&) {
                if (!changed) { changed = true; controller.removeClip(track, second); }
                return true;
            }, report);
        check(stale.isOk() && report.cancelled && fs::is_empty(renderDir) &&
              controller.audioClip(track, first)->offlineHistory.empty() && !controller.audioClip(track, second),
              "a project edit during progress cancels safely without overwriting the edit");
        rack.shutdown();
    }

    // Source trims, stretch and gain are printed once and fully recoverable;
    // history media is never treated as disposable cache or shared local paths.
    {
        daw::EngineController controller;
        controller.initialize(48000, 256, false);
        const auto track = controller.addTrack(daw::TrackKind::Audio, "Edited audio");
        const auto id = controller.importAudio(tone, track, 1.0);
        auto project = controller.project();
        auto& clip = project.tracks.front().clips.front();
        clip.offsetSeconds = 0.05;
        clip.durationSeconds = 0.3;
        clip.sampleEdit.stretchTime = 2.0;
        clip.sampleEdit.reverse = true;
        clip.fadeInSeconds = 0.02;
        clip.fadeOutSeconds = 0.03;
        clip.gain = 0.6f;
        clip.pan = -0.2f;
        controller.restoreProject(project, "Edited source");
        daw::EngineController rack;
        rack.initialize(48000, 256, false);
        const auto rackTrack = rack.addTrack(daw::TrackKind::Audio, "Rack");
        rack.addInsert(rackTrack, daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
        const auto chain = rack.copyChannelStrip(rackTrack, false);
        daw::EngineController::OfflineRenderReport report;
        const daw::EngineController::ClipAddress address{track, id};
        check(controller.renderClipsOffline({address}, chain, false, {}, report).isOk(),
              "a trimmed stretched reversed clip renders");
        const auto* rendered = controller.audioClip(track, id);
        check(std::abs(rendered->durationSeconds - 0.3) < 1e-5 && rendered->offsetSeconds == 0.0 &&
              rendered->sampleEdit.stretchTime == 1.0 && !rendered->sampleEdit.reverse &&
              rendered->fadeInSeconds == 0.0 && rendered->gain == 1.0f && rendered->pan == 0.0f,
              "rendered file has exact edited duration and neutral playback settings");
        const auto renderedId = rendered->offlineVersionId;
        check(controller.restoreOfflineRenderOriginal(address).isOk(), "restore edited original");
        const auto* original = controller.audioClip(track, id);
        check(original->offsetSeconds == 0.05 && original->sampleEdit.stretchTime == 2.0 &&
              original->sampleEdit.reverse && original->fadeInSeconds == 0.02 && original->gain == 0.6f,
              "original version restores its trim, stretch, reverse, fades and gain");
        controller.selectOfflineRenderVersion(address, renderedId);
        auto missing = controller.project();
        missing.tracks.front().clips.front().offlineHistory.front().source.filePath = (dir / "missing.wav").string();
        controller.restoreProject(missing, "Missing original");
        check(!controller.restoreOfflineRenderOriginal(address) &&
              controller.audioClip(track, id)->offlineVersionId == renderedId,
              "missing history media never replaces playable audio with silence");
        const auto projected = daw::cloud::projectForCloudSnapshotV1(controller.project());
        check(!projected.valid() && !daw::cloud::containsLocalPathOrUiState(projected.document) &&
              !controller.audioClip(track, id)->offlineHistory.empty(),
              "local offline history blocks cloud publication without leaking paths or changing the local project");
    }

    // History protects every original comp/take file from explicit take cleanup.
    {
        daw::EngineController controller;
        controller.initialize(48000, 256, false);
        const auto track = controller.addTrack(daw::TrackKind::Audio, "Comp history");
        const auto id = controller.importAudio(tone, track, 0.0);
        const auto takeFile = (dir / "original-take.wav").string();
        writeTone(takeFile);
        const auto take = controller.addTakeFromFile(track, id, takeFile);
        const auto original = *controller.audioClip(track, id);
        daw::EngineController rack;
        rack.initialize(48000, 256, false);
        const auto rackTrack = rack.addTrack(daw::TrackKind::Audio, "Rack");
        rack.addInsert(rackTrack, daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
        daw::EngineController::OfflineRenderReport report;
        const daw::EngineController::ClipAddress address{track, id};
        check(controller.renderClipsOffline({address}, rack.copyChannelStrip(rackTrack, false), false, {}, report).isOk() &&
              controller.audioClip(track, id)->takes.empty(), "comp renders into one audio source");
        check(controller.restoreOfflineRenderOriginal(address).isOk() &&
              controller.audioClip(track, id)->takes.size() == original.takes.size() &&
              controller.audioClip(track, id)->comp.front().takeId == original.comp.front().takeId,
              "history restores the original comp and its takes");
        controller.removeTake(track, id, take, true);
        check(fs::exists(takeFile) && controller.restoreOfflineRenderOriginal(address).isOk(),
              "deleting a take cannot delete media held by offline history");
        auto unused = controller.project();
        auto& compClip = unused.tracks.front().clips.front();
        for (auto& segment : compClip.comp) segment.takeId = compClip.takes.front().id;
        controller.restoreProject(unused, "Unused take");
        check(controller.deleteUnusedTakes(true) > 0 && fs::exists(takeFile) &&
              controller.restoreOfflineRenderOriginal(address).isOk(),
              "unused-take cleanup preserves historical audio");
        const auto package = (dir / "comp-history.vlt").string();
        check(controller.saveProject(package).isOk(), "comp history package saves");
        daw::EngineController reopened;
        reopened.initialize(48000, 256, false);
        check(reopened.openProject(package).isOk() && reopened.restoreOfflineRenderOriginal(address).isOk(),
              "comp history and take media reopen from the portable package");
    }

    // Pure document codec remains additive: old documents default the new
    // fields, while v8 preserves an explicit semantic injection point.
    {
        daw::ProjectModel project;
        daw::TrackModel track;
        track.id = "track";
        track.kind = daw::TrackKind::Audio;
        daw::ClipModel clip;
        clip.id = "clip";
        clip.kind = daw::ClipKind::Audio;
        clip.filePath = tone;
        clip.durationSeconds = 0.25;
        clip.playbackInjection = {
            daw::PlaybackInjectionStage::BeforeMasterFader, "master"};
        track.clips.push_back(clip);
        project.tracks.push_back(track);
        std::string json;
        check(daw::ProjectSerializer::serializeDocument(project, json).isOk(),
              "v8 document serializes bounce routing");
        daw::ProjectModel decoded;
        check(daw::ProjectSerializer::deserializeDocument(decoded, json).isOk(),
              "v8 document deserializes bounce routing");
        check(!decoded.tracks.empty() && !decoded.tracks.front().clips.empty() &&
                  decoded.tracks.front().clips.front().playbackInjection.stage ==
                      daw::PlaybackInjectionStage::BeforeMasterFader,
              "semantic playback injection round-trips");

        auto old = nlohmann::json::parse(json);
        old["version"] = 7;
        auto& oldClip = old["tracks"][0]["clips"][0];
        oldClip.erase("playbackInjection");
        oldClip.erase("offlineProcess");
        daw::ProjectModel legacy;
        check(daw::ProjectSerializer::deserializeDocument(
                  legacy, old.dump()).isOk(),
              "v7 document still loads with empty processing fields");
        check(!legacy.tracks.front().clips.front().playbackInjection.active() &&
                  legacy.tracks.front().clips.front().offlineProcess.empty(),
              "legacy defaults are non-processing and non-injected");
    }

    fs::remove_all(dir);
    if (failures) std::printf("FAILURES PRESENT: %d\n", failures);
    return failures ? 1 : 0;
}
