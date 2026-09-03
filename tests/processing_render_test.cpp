#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Internal/EqualizerInstance.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
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

    // A real built-in effect exercises state capture, cache playback and the
    // package's Content/State round-trip without depending on machine plugins.
    {
        daw::EngineController controller;
        check(controller.initialize(48000, 256, false).isOk(),
              "offline controller initializes headless");
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Offline Source");
        const std::string clip = controller.importAudio(tone, track, 0.0);
        const daw::EngineController::ClipAddress address{track, clip};

        daw::EngineController rack;
        check(rack.initialize(48000, 256, false).isOk(),
              "scratch offline rack initializes without a device");
        const std::string rackTrack =
            rack.addTrack(daw::TrackKind::Audio, "Rack");
        const std::string equalizer = rack.addInsert(
            rackTrack,
            daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
        check(!equalizer.empty(), "built-in effect loads into scratch rack");
        auto chain = rack.copyChannelStrip(rackTrack, false);

        daw::EngineController::OfflineRenderReport rendered;
        const audio::Result renderResult =
            controller.renderClipsOffline({address}, chain, false, {}, rendered);
        check(renderResult.isOk() && rendered.files.size() == 1,
              "Offline Render creates a processed cache");
        check(controller.offlineProcessCacheValid(address),
              "fresh processed cache fingerprint is valid");
        const daw::ClipModel* processed = findClip(controller, track, clip);
        check(processed && processed->offlineProcess.chain.size() == 1 &&
                  fs::exists(processed->offlineProcess.renderedFilePath),
              "offline chain and generated audio stay attached to the clip");

        daw::EngineController::OfflineRenderReport tailed;
        check(controller.renderClipsOffline({address}, chain, true, {}, tailed)
                  .isOk(),
              "Offline Render can include a silence-detected tail");
        processed = findClip(controller, track, clip);
        check(processed &&
                  processed->offlineProcess.renderedDurationSeconds >
                      processed->durationSeconds &&
                  controller.clipPlaybackDuration(*processed) ==
                      processed->offlineProcess.renderedDurationSeconds,
              "tail extends the visible and playable clip duration");

        const daw::recovery::RecoverySnapshot recovery =
            controller.captureRecoverySnapshot();
        const daw::TrackModel* recoveryTrack = recovery.project.findTrack(track);
        const auto recoveryClip =
            recoveryTrack
                ? std::find_if(recoveryTrack->clips.begin(),
                               recoveryTrack->clips.end(),
                               [&](const daw::ClipModel& item) {
                                   return item.id == clip;
                               })
                : std::vector<daw::ClipModel>::const_iterator{};
        check(recoveryTrack && recoveryClip != recoveryTrack->clips.end() &&
                  recoveryClip->offlineProcess.chain.size() == 1 &&
                  !recoveryClip->offlineProcess.renderedFilePath.empty(),
              "recovery snapshot retains offline chain and managed cache");

        const std::string package = (dir / "processed.vlt").string();
        check(controller.saveProject(package).isOk(),
              "v8 project package saves offline processing");
        daw::EngineController reopened;
        reopened.initialize(48000, 256, false);
        check(reopened.openProject(package).isOk(),
              "v8 project package reloads offline processing");
        const daw::ClipModel* reloaded = findClip(reopened, track, clip);
        check(reloaded && reloaded->offlineProcess.chain.size() == 1 &&
                  reopened.offlineProcessCacheValid(address),
              "processed cache and opaque chain state survive reopen");
        check(reloaded &&
                  reloaded->offlineProcess.renderedFilePath.find("Content") !=
                      std::string::npos,
              "processed audio is resolved from package Content");

        controller.setClipGain(track, clip, 0.5f);
        check(!controller.offlineProcessCacheValid(address),
              "clip gain change invalidates the processed cache");

        chain.inserts.front().model.bypassed = true;
        daw::EngineController::OfflineRenderReport bypassed;
        check(controller.renderClipsOffline({address}, chain, false, {}, bypassed)
                  .isOk(),
              "fully bypassed offline chain returns to original");
        processed = findClip(controller, track, clip);
        check(processed && processed->offlineProcess.renderedFilePath.empty() &&
                  processed->offlineProcess.chain.size() == 1 &&
                  processed->offlineProcess.chain.front().bypassed,
              "disabled chain is retained while processed cache is removed");

        auto unavailable = chain;
        unavailable.inserts.front().model.bypassed = false;
        unavailable.inserts.front().model.format = daw::PluginFormat::Vst3;
        unavailable.inserts.front().model.uid = "missing.test.plugin";
        unavailable.inserts.front().model.name = "Missing Test Plugin";
        const auto beforeMissing =
            controller.offlineProcessChain(address).inserts.size();
        daw::EngineController::OfflineRenderReport blocked;
        const audio::Result missing = controller.renderClipsOffline(
            {address}, unavailable, false, {}, blocked);
        check(!missing && missing.error() == audio::EngineError::FileNotFound,
              "enabled missing plugin blocks a re-render");
        check(controller.offlineProcessChain(address).inserts.size() ==
                  beforeMissing,
              "failed offline render is atomic");
        rack.shutdown();
    }

    // Cancellation after clip one has rendered still discards every staged
    // file and leaves neither clip with a partial chain/cache mutation.
    {
        daw::EngineController controller;
        controller.initialize(48000, 256, false);
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Atomic Offline");
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
        check(firstAfter && secondAfter && firstAfter->offlineProcess.empty() &&
                  secondAfter->offlineProcess.empty(),
              "later cancellation rolls back the complete offline batch");
        rack.shutdown();
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
