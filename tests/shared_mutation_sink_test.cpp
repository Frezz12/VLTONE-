#include "EngineController.hpp"
#include "ChannelStripPreset.hpp"
#include "collaboration/MutationCapabilityLedger.hpp"
#include "collaboration/CommandJson.hpp"
#include "collaboration/SharedAssetMutationSink.hpp"
#include "collaboration/SharedMutationSink.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

int failures = 0;

bool check(bool condition, const char* description) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) ++failures;
    return condition;
}

class FakeSharedMutationSink final : public daw::collab::SharedMutationSink {
public:
    daw::collab::SharedMutationResult result =
        daw::collab::SharedMutationResult::Submitted;
    bool cloudBinding = true;

    int timeSignatureCalls = 0;
    int numerator = 0;
    int denominator = 0;

    int projectKeyCalls = 0;
    int keyRoot = 0;
    std::string scale;

    int aiInstructionsCalls = 0;
    std::string aiInstructions;

    int renameTrackCalls = 0;
    std::string renamedTrackId;
    std::string trackName;

    int trackMutedCalls = 0;
    std::string mutedTrackId;
    bool muted = false;

    int tracksMutedCalls = 0;
    std::vector<std::string> mutedTrackIds;
    bool tracksMuted = false;

    int clearAllMutesCalls = 0;
    std::vector<std::string> clearedTrackIds;
    int genericCalls = 0;
    std::vector<daw::collab::CommandBody> genericBodies;

    bool handlesCloudBinding() override { return cloudBinding; }

    daw::collab::SharedMutationResult submit(
        daw::collab::SharedMutationRequest request) override {
        ++genericCalls;
        genericBodies.push_back(std::move(request.body));
        return result;
    }

    daw::collab::SharedMutationResult setTimeSignature(
        int nextNumerator, int nextDenominator) override {
        ++timeSignatureCalls;
        numerator = nextNumerator;
        denominator = nextDenominator;
        return result;
    }

    daw::collab::SharedMutationResult setProjectKey(
        int root, std::string_view scaleId) override {
        ++projectKeyCalls;
        keyRoot = root;
        scale = scaleId;
        return result;
    }

    daw::collab::SharedMutationResult setAiInstructions(
        std::string_view text) override {
        ++aiInstructionsCalls;
        aiInstructions = text;
        return result;
    }

    daw::collab::SharedMutationResult renameTrack(
        std::string_view trackId, std::string_view name) override {
        ++renameTrackCalls;
        renamedTrackId = trackId;
        trackName = name;
        return result;
    }

    daw::collab::SharedMutationResult setTrackMuted(
        std::string_view trackId, bool nextMuted) override {
        ++trackMutedCalls;
        mutedTrackId = trackId;
        muted = nextMuted;
        return result;
    }

    daw::collab::SharedMutationResult setTracksMuted(
        std::span<const std::string> trackIds, bool nextMuted) override {
        ++tracksMutedCalls;
        mutedTrackIds.assign(trackIds.begin(), trackIds.end());
        tracksMuted = nextMuted;
        return result;
    }

    daw::collab::SharedMutationResult clearAllMutes(
        std::span<const std::string> mutedTrackIds) override {
        ++clearAllMutesCalls;
        clearedTrackIds.assign(mutedTrackIds.begin(), mutedTrackIds.end());
        return result;
    }
};

class FakeSharedAssetMutationSink final
    : public daw::collab::SharedAssetMutationSink {
public:
    daw::collab::SharedMutationResult result =
        daw::collab::SharedMutationResult::Submitted;
    std::vector<daw::collab::SharedAssetMutationRequest> requests;

    daw::collab::SharedMutationResult prepare(
        daw::collab::SharedAssetMutationRequest request) override {
        requests.push_back(std::move(request));
        return result;
    }
};

std::filesystem::path writeSharedAssetTone() {
    constexpr audio::SampleRate rate = 48000.0;
    constexpr audio::BufferSize frames = 480;
    const auto path = std::filesystem::temp_directory_path() /
        ("vlt-shared-asset-" + daw::newUuid() + ".wav");
    audio::AudioBuffer tone(2, frames);
    for (audio::BufferSize frame = 0; frame < frames; ++frame) {
        const float sample = 0.25f * std::sin(
            2.0f * 3.14159265f * 440.0f * float(frame) / float(rate));
        tone.getChannel(0)[frame] = sample;
        tone.getChannel(1)[frame] = sample;
    }
    audio::AudioRecorder recorder;
    recorder.initialize(rate, 2);
    if (!recorder.writeWAVFile(path.string(), tone, rate)) return {};
    return path;
}

daw::AssetRef verifiedAsset(
    const daw::collab::SharedAssetMutationRequest& request) {
    daw::AssetRef asset;
    asset.assetId = request.assetId;
    asset.sha256 = std::string(64, 'a');
    asset.kind = daw::AssetKind::Audio;
    std::error_code error;
    asset.byteSize = std::filesystem::file_size(request.sourcePath, error);
    asset.originalName = request.displayName;
    asset.mimeType = request.contentType;
    asset.codec = request.codec;
    asset.sampleRate = request.sampleRate;
    asset.channels = request.channels;
    asset.frames = request.frames;
    return asset;
}

bool jsonContainsString(const nlohmann::json& value,
                        const std::string& needle) {
    if (value.is_string()) return value.get<std::string>() == needle;
    if (!value.is_array() && !value.is_object()) return false;
    return std::any_of(value.begin(), value.end(),
                       [&](const nlohmann::json& child) {
                           return jsonContainsString(child, needle);
                       });
}

bool commandContainsString(const daw::collab::CommandBody& body,
                           const std::string& needle) {
    daw::collab::ProjectCommand command;
    command.meta.operationId = daw::newUuid();
    command.body = body;
    return jsonContainsString(daw::collab::projectCommandToJson(command),
                              needle);
}

void verifySharedAssetMutationGate() {
    const std::filesystem::path tone = writeSharedAssetTone();
    check(!tone.empty(), "shared asset fixture writes a WAV");
    if (tone.empty()) return;

    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "shared asset fixture initializes");
    const std::string trackId =
        controller.addTrack(daw::TrackKind::Audio, "Audio");
    const daw::ProjectModel before = controller.project();
    const std::size_t undoDepth = controller.undoDepth();

    FakeSharedMutationSink commands;
    FakeSharedAssetMutationSink assets;
    controller.attachSharedMutationSink(commands);
    controller.attachSharedAssetMutationSink(assets);

    // Locates the AddClip / SetClipAsset a submitted body carries, so the two
    // halves of an optimistic import can be told apart structurally rather
    // than by counting.
    const auto addClipIn = [](const daw::collab::CommandBody& body,
                              const std::string& clip)
        -> const daw::collab::AddClip* {
        const auto* batch =
            std::get_if<std::shared_ptr<daw::collab::BatchCommand>>(&body);
        if (!batch || !*batch) return nullptr;
        for (const auto& child : (*batch)->commands) {
            const auto* value = std::get_if<daw::collab::AddClip>(&child.body);
            if (value && value->clipId == clip) return value;
        }
        return nullptr;
    };
    const auto bodyHasSetClipAsset = [](const daw::collab::CommandBody& body) {
        if (std::get_if<daw::collab::SetClipAsset>(&body)) return true;
        const auto* batch =
            std::get_if<std::shared_ptr<daw::collab::BatchCommand>>(&body);
        if (!batch || !*batch) return false;
        for (const auto& child : (*batch)->commands) {
            if (std::get_if<daw::collab::SetClipAsset>(&child.body))
                return true;
        }
        return false;
    };

    const std::string clipId =
        controller.importAudio(tone.string(), trackId, 0.25);
    const daw::collab::AddClip* sharedClip =
        commands.genericCalls == 1
            ? addClipIn(commands.genericBodies.front(), clipId)
            : nullptr;
    // The clip is shared the moment it is dropped, carrying no asset: that is
    // what lets it appear for every participant while the bytes are still
    // uploading. It must still reach the document only through the command
    // path, with no local mutation and no legacy undo entry.
    check(!clipId.empty() && assets.requests.size() == 1 &&
              commands.genericCalls == 1 && sharedClip &&
              sharedClip->trackId == trackId &&
              sharedClip->kind == daw::ClipKind::Audio &&
              !bodyHasSetClipAsset(commands.genericBodies.front()) &&
              !commandContainsString(commands.genericBodies.front(),
                                     assets.requests.front().sourcePath) &&
              controller.project().findTrack(trackId)->clips.empty() &&
              controller.undoDepth() == undoDepth,
          "cloud import shares the clip immediately without its asset");

    const auto request = assets.requests.front();
    const daw::AssetRef ready = verifiedAsset(request);
    const auto completed =
        controller.completeSharedAssetMutation(request.requestId, ready);
    const daw::collab::SetClipAsset* assetCommand =
        commands.genericBodies.size() == 2
            ? std::get_if<daw::collab::SetClipAsset>(
                  &commands.genericBodies.back())
            : nullptr;
    // The verified upload binds the asset as its own operation, so every
    // participant's already-visible clip becomes audible at that point.
    check(completed == daw::collab::SharedMutationResult::Submitted &&
              commands.genericCalls == 2 && assetCommand &&
              assetCommand->trackId == trackId &&
              assetCommand->clipId == clipId &&
              assetCommand->asset == ready &&
              !commandContainsString(commands.genericBodies.back(),
                                     request.sourcePath) &&
              controller.project().tracks.size() == before.tracks.size() &&
              controller.project().findTrack(trackId)->clips.empty() &&
              controller.undoDepth() == undoDepth,
          "verified upload binds the asset as its own path-free operation");
    check(controller.completeSharedAssetMutation(request.requestId, ready) ==
                  daw::collab::SharedMutationResult::Blocked &&
              commands.genericCalls == 2,
          "duplicate verified callback cannot submit twice");

    const std::string cancelled =
        controller.importAudio(tone.string(), trackId, 0.5);
    const auto cancelledRequest = assets.requests.back();
    controller.cancelSharedAssetMutation(cancelledRequest.requestId);
    check(!cancelled.empty() && commands.genericCalls == 3 &&
              controller.completeSharedAssetMutation(
                  cancelledRequest.requestId,
                  verifiedAsset(cancelledRequest)) ==
                  daw::collab::SharedMutationResult::Blocked &&
              commands.genericCalls == 3,
          "cancelled generation ignores a late verified callback");

    const std::string invalid =
        controller.importAudio(tone.string(), trackId, 0.6);
    const auto invalidRequest = assets.requests.back();
    daw::AssetRef wrongMetadata = verifiedAsset(invalidRequest);
    ++wrongMetadata.frames;
    check(!invalid.empty() && commands.genericCalls == 4 &&
              controller.completeSharedAssetMutation(
                  invalidRequest.requestId, wrongMetadata) ==
                  daw::collab::SharedMutationResult::Blocked &&
              controller.completeSharedAssetMutation(
                  invalidRequest.requestId,
                  verifiedAsset(invalidRequest)) ==
                  daw::collab::SharedMutationResult::Blocked &&
              commands.genericCalls == 4,
          "mismatched verified metadata is terminal and cannot submit");

    // The upload is reserved before the clip is shared, so a refusal leaves no
    // orphaned silent clip behind.
    assets.result = daw::collab::SharedMutationResult::Blocked;
    check(controller.importAudio(tone.string(), trackId, 0.75).empty() &&
              commands.genericCalls == 4 &&
              controller.project().findTrack(trackId)->clips.empty(),
          "synchronous asset rejection shares nothing at all");

    check(controller.detachSharedAssetMutationSink(assets) &&
              controller.detachSharedMutationSink(commands),
          "shared asset test sinks detach");
    std::error_code removeError;
    std::filesystem::remove(tone, removeError);
}

void verifyGenericMutationRoutes(daw::collab::SharedMutationResult result) {
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "generic command fixture initializes");
    const std::string track =
        controller.addTrack(daw::TrackKind::Audio, "Track");
    const auto before = controller.project();
    const std::size_t undoDepth = controller.undoDepth();

    FakeSharedMutationSink sink;
    sink.result = result;
    sink.cloudBinding =
        result != daw::collab::SharedMutationResult::LocalFallback;
    controller.attachSharedMutationSink(sink);
    const std::string folder = controller.addFolder(true, "Group");
    check(sink.genericCalls == 1 && sink.genericBodies.size() == 1 &&
              std::holds_alternative<
                  std::shared_ptr<daw::collab::BatchCommand>>(
                  sink.genericBodies.front()) &&
              (result == daw::collab::SharedMutationResult::Submitted
                   ? !folder.empty()
                   : folder.empty()),
          "folder creation offers exactly one outer command batch");

    controller.setTrackColor(track, 0x123456u);
    check(sink.genericCalls == 2 &&
              controller.project().findTrack(track)->color ==
                  before.findTrack(track)->color &&
              controller.project().tracks.size() == before.tracks.size() &&
              controller.undoDepth() == undoDepth,
          "submitted/blocked generic mutations do not use local document or undo");
    check(controller.detachSharedMutationSink(sink),
          "generic command sink detaches");
}

void verifyTakeMoveRoutes(daw::collab::SharedMutationResult result) {
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "take move fixture initializes");
    daw::ProjectModel project;
    daw::TrackModel track;
    track.id = "take-track";
    track.kind = daw::TrackKind::Audio;
    daw::ClipModel clip;
    clip.id = "take-clip";
    clip.kind = daw::ClipKind::Audio;
    clip.durationSeconds = 1.0;
    for (const char* id : {"take-a", "take-b", "take-c"}) {
        daw::TakeModel take;
        take.id = id;
        take.name = id;
        clip.takes.push_back(std::move(take));
    }
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
    check(controller.materializeCollaborationProject(std::move(project), true)
              .isOk(),
          "take move fixture materializes");

    FakeSharedMutationSink sink;
    sink.result = result;
    controller.attachSharedMutationSink(sink);
    const std::size_t undoDepth = controller.undoDepth();
    controller.moveTake("take-track", "take-clip", "take-a", 2);

    const auto* unchanged = controller.project().findTrack("take-track");
    const auto* command = sink.genericBodies.size() == 1
                              ? std::get_if<daw::collab::MoveTake>(
                                    &sink.genericBodies.front())
                              : nullptr;
    check(sink.genericCalls == 1 && command &&
              command->trackId == "take-track" &&
              command->clipId == "take-clip" &&
              command->takeId == "take-a" &&
              command->afterId == "take-c",
          "take move publishes one stable-anchor command");
    check(unchanged && unchanged->clips.front().takes.front().id == "take-a" &&
              controller.undoDepth() == undoDepth,
          result == daw::collab::SharedMutationResult::Submitted
              ? "submitted take move avoids local mutation and legacy undo"
              : "blocked take move avoids local mutation and legacy undo");
    check(controller.detachSharedMutationSink(sink),
          "take move sink detaches");
}

bool allBatches(const FakeSharedMutationSink& sink, int firstCall) {
    if (sink.genericCalls <= firstCall ||
        sink.genericBodies.size() != std::size_t(sink.genericCalls)) {
        return false;
    }
    return std::all_of(
        sink.genericBodies.begin() + firstCall, sink.genericBodies.end(),
        [](const daw::collab::CommandBody& body) {
            const auto* batch = std::get_if<
                std::shared_ptr<daw::collab::BatchCommand>>(&body);
            return batch && *batch && !(*batch)->commands.empty();
        });
}

daw::AssetRef sharedAudioAsset() {
    daw::AssetRef asset;
    asset.assetId = daw::newUuid();
    asset.sha256.assign(64, 'a');
    asset.kind = daw::AssetKind::Audio;
    asset.byteSize = 4096;
    asset.originalName = "sample.wav";
    asset.mimeType = "audio/wav";
    asset.codec = "pcm_f32le";
    asset.sampleRate = 48000.0;
    asset.channels = 1;
    asset.frames = 1024;
    return asset;
}

void verifyVerifiedAssetActions() {
    const std::filesystem::path tone = writeSharedAssetTone();
    check(!tone.empty(), "verified asset action fixture writes a WAV");
    if (tone.empty()) return;

    const auto finishLatest = [&](daw::EngineController& controller,
                                  FakeSharedAssetMutationSink& assets,
                                  FakeSharedMutationSink& commands,
                                  int commandsBefore) {
        if (assets.requests.empty()) return false;
        const auto& request = assets.requests.back();
        return controller.completeSharedAssetMutation(
                   request.requestId, verifiedAsset(request)) ==
                   daw::collab::SharedMutationResult::Submitted &&
               commands.genericCalls == commandsBefore + 1 &&
               !commandContainsString(commands.genericBodies.back(),
                                      request.sourcePath);
    };

    {
        daw::EngineController controller;
        controller.initialize(48000.0, 512, false);
        FakeSharedMutationSink commands;
        FakeSharedAssetMutationSink assets;
        controller.attachSharedMutationSink(commands);
        controller.attachSharedAssetMutationSink(assets);
        const std::string track = controller.importAudioToNewTrack(
            tone.string(), 0.0, "Imported");
        // The track and its clip are shared immediately, as one batch that
        // carries no asset; the verified asset follows as a bare setAsset.
        const bool sharedNow =
            !track.empty() && commands.genericCalls == 1 &&
            std::holds_alternative<std::shared_ptr<daw::collab::BatchCommand>>(
                commands.genericBodies.back()) &&
            controller.project().tracks.empty();
        check(sharedNow && finishLatest(controller, assets, commands, 1) &&
                  std::holds_alternative<daw::collab::SetClipAsset>(
                      commands.genericBodies.back()),
              "new-track import shares the track now and its asset later");
        controller.detachSharedAssetMutationSink(assets);
        controller.detachSharedMutationSink(commands);
    }

    {
        daw::EngineController controller;
        controller.initialize(48000.0, 512, false);
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Comp drift");
        const std::string clip =
            controller.importAudio(tone.string(), track, 0.0);
        daw::ProjectModel project = controller.project();
        auto* source = project.findTrack(track);
        if (source && !source->clips.empty()) {
            auto& sourceClip = source->clips.front();
            sourceClip.asset = sharedAudioAsset();
            daw::TakeModel base;
            base.id = daw::newUuid();
            base.name = "Take 1";
            base.lengthSeconds = sourceClip.durationSeconds;
            base.channels = sourceClip.channels;
            base.asset = sourceClip.asset;
            sourceClip.takes.push_back(base);
            sourceClip.comp.push_back({
                base.id, 0.0, sourceClip.durationSeconds, daw::newUuid()});
        }
        controller.materializeCollaborationProject(std::move(project), true);
        FakeSharedMutationSink commands;
        FakeSharedAssetMutationSink assets;
        controller.attachSharedMutationSink(commands);
        controller.attachSharedAssetMutationSink(assets);
        const std::string take = controller.addTakeFromFile(
            track, clip, tone.string(), 0.0, "Queued take");
        daw::ProjectModel remote = controller.project();
        auto* remoteTrack = remote.findTrack(track);
        if (remoteTrack && !remoteTrack->clips.empty() &&
            !remoteTrack->clips.front().comp.empty()) {
            remoteTrack->clips.front().comp.front().endSeconds *= 0.5;
        }
        controller.materializeCollaborationProject(std::move(remote), true);
        const bool queued = !take.empty() && !assets.requests.empty();
        const auto completed = queued
            ? controller.completeSharedAssetMutation(
                  assets.requests.back().requestId,
                  verifiedAsset(assets.requests.back()))
            : daw::collab::SharedMutationResult::Submitted;
        check(queued &&
                  completed == daw::collab::SharedMutationResult::Blocked &&
                  commands.genericCalls == 0,
              "verified take import rejects a stale remote comp baseline");
        controller.detachSharedAssetMutationSink(assets);
        controller.detachSharedMutationSink(commands);
    }

    {
        daw::EngineController controller;
        controller.initialize(48000.0, 512, false);
        const std::string track = controller.addTrack(
            daw::TrackKind::Instrument, "Instrument");
        const std::size_t undoDepth = controller.undoDepth();
        FakeSharedMutationSink commands;
        FakeSharedAssetMutationSink assets;
        controller.attachSharedMutationSink(commands);
        controller.attachSharedAssetMutationSink(assets);
        const bool queued = controller.loadInstrumentSampler(track,
                                                              tone.string());
        check(queued && commands.genericCalls == 0 &&
                  controller.project().findTrack(track)->instrument.id.empty() &&
                  finishLatest(controller, assets, commands, 0) &&
                  std::holds_alternative<daw::collab::AddPluginInsert>(
                      commands.genericBodies.back()) &&
                  controller.undoDepth() == undoDepth,
              "Sampler creation publishes one verified typed command");
        controller.detachSharedAssetMutationSink(assets);
        controller.detachSharedMutationSink(commands);
    }

    {
        daw::EngineController controller;
        controller.initialize(48000.0, 512, false);
        const std::string track = controller.addTrack(
            daw::TrackKind::Instrument, "Sampler");
        const auto sampler = controller.pluginManager().find(
            daw::plugins::Format::Internal, "daw.sampler");
        const bool installed = sampler &&
            controller.setTrackInstrumentPlugin(track, *sampler);
        const std::string slot = installed
            ? controller.project().findTrack(track)->instrument.id
            : std::string{};
        FakeSharedMutationSink commands;
        FakeSharedAssetMutationSink assets;
        controller.attachSharedMutationSink(commands);
        controller.attachSharedAssetMutationSink(assets);
        const bool queued = installed &&
            controller.loadSamplerSample(track, slot, tone.string());
        check(queued && commands.genericCalls == 0 &&
                  finishLatest(controller, assets, commands, 0) &&
                  std::holds_alternative<
                      daw::collab::SetPluginAssetBinding>(
                      commands.genericBodies.back()) &&
                  controller.project()
                      .findTrack(track)
                      ->instrument.assetBindings.empty(),
              "existing Sampler binds a verified sample without local state");
        controller.detachSharedAssetMutationSink(assets);
        controller.detachSharedMutationSink(commands);
    }

    {
        daw::EngineController controller;
        controller.initialize(48000.0, 512, false);
        const std::string pattern = controller.addPattern("Pattern");
        const std::size_t tracksBefore = controller.project().tracks.size();
        FakeSharedMutationSink commands;
        FakeSharedAssetMutationSink assets;
        controller.attachSharedMutationSink(commands);
        controller.attachSharedAssetMutationSink(assets);
        const std::string lane = controller.addPatternSample(
            pattern, tone.string(), 0.0);
        check(!lane.empty() && commands.genericCalls == 0 &&
                  controller.project().tracks.size() == tracksBefore &&
                  finishLatest(controller, assets, commands, 0) &&
                  std::holds_alternative<std::shared_ptr<
                      daw::collab::BatchCommand>>(commands.genericBodies.back()),
              "Pattern sample publishes one verified outer batch");
        controller.detachSharedAssetMutationSink(assets);
        controller.detachSharedMutationSink(commands);
    }

    {
        daw::EngineController controller;
        controller.initialize(48000.0, 512, false);
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Takes");
        const std::string clip =
            controller.importAudio(tone.string(), track, 0.0);
        daw::ProjectModel project = controller.project();
        auto* source = project.findTrack(track);
        if (source && !source->clips.empty())
            source->clips.front().asset = sharedAudioAsset();
        controller.materializeCollaborationProject(std::move(project), true);
        const std::size_t undoDepth = controller.undoDepth();
        FakeSharedMutationSink commands;
        FakeSharedAssetMutationSink assets;
        controller.attachSharedMutationSink(commands);
        controller.attachSharedAssetMutationSink(assets);
        const std::string take = controller.addTakeFromFile(
            track, clip, tone.string(), 0.0, "Take 2");
        check(!take.empty() && commands.genericCalls == 0 &&
                  controller.project().findTrack(track)->clips.front()
                      .takes.empty() &&
                  finishLatest(controller, assets, commands, 0) &&
                  std::holds_alternative<std::shared_ptr<
                      daw::collab::BatchCommand>>(commands.genericBodies.back()) &&
                  controller.undoDepth() == undoDepth,
              "take import promotes and comps only inside one verified batch");
        controller.detachSharedAssetMutationSink(assets);
        controller.detachSharedMutationSink(commands);
    }

    {
        daw::EngineController controller;
        controller.initialize(48000.0, 512, false);
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Flatten");
        const std::string clip =
            controller.importAudio(tone.string(), track, 0.0);
        controller.addTakeFromFile(track, clip, tone.string(), 0.0,
                                   "Layer");
        daw::ProjectModel project = controller.project();
        auto* source = project.findTrack(track);
        if (source && !source->clips.empty()) {
            source->clips.front().asset = sharedAudioAsset();
            for (daw::TakeModel& take : source->clips.front().takes)
                take.asset = sharedAudioAsset();
        }
        controller.materializeCollaborationProject(std::move(project), true);
        const std::size_t takesBefore = controller.project()
            .findTrack(track)->clips.front().takes.size();
        FakeSharedMutationSink commands;
        FakeSharedAssetMutationSink assets;
        controller.attachSharedMutationSink(commands);
        controller.attachSharedAssetMutationSink(assets);
        const std::string flattened = controller.flattenComp(track, clip);
        const std::string rendered = assets.requests.empty()
            ? std::string{}
            : assets.requests.back().sourcePath;
        const bool renderedBeforeVerify = !rendered.empty() &&
            std::filesystem::exists(rendered);
        check(!flattened.empty() && commands.genericCalls == 0 &&
                  controller.project().findTrack(track)->clips.front()
                          .takes.size() == takesBefore &&
                  renderedBeforeVerify &&
                  finishLatest(controller, assets, commands, 0) &&
                  !std::filesystem::exists(rendered) &&
                  std::holds_alternative<std::shared_ptr<
                      daw::collab::BatchCommand>>(commands.genericBodies.back()),
              "flatten render is uploaded, submitted once, then cleaned");
        controller.detachSharedAssetMutationSink(assets);
        controller.detachSharedMutationSink(commands);
    }

    std::error_code removeError;
    std::filesystem::remove(tone, removeError);
}

void verifySamplerBatchMutators(daw::collab::SharedMutationResult result) {
    const bool submitted = result == daw::collab::SharedMutationResult::Submitted;
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "shared Sampler fixture initializes");
    const auto sampler = controller.pluginManager().find(
        daw::plugins::Format::Internal, "daw.sampler");
    const std::string pattern = controller.addPattern("Pattern");
    const std::string loadedTrack =
        controller.addTrack(daw::TrackKind::Instrument, "Loaded Sampler");
    check(sampler && !pattern.empty() && !loadedTrack.empty() &&
              controller.setTrackInstrumentPlugin(loadedTrack, *sampler),
          "shared Sampler fixture materializes empty instruments");
    daw::ProjectModel project = controller.project();
    daw::TrackModel* loaded = project.findTrack(loadedTrack);
    if (loaded) {
        loaded->instrument.assetBindings.push_back(
            {"sample", sharedAudioAsset(), true});
    }
    check(loaded &&
              controller.materializeCollaborationProject(std::move(project),
                                                         true)
                  .isOk(),
          "shared Sampler fixture binds one durable sample");
    const std::string slotId =
        controller.project().findTrack(loadedTrack)->instrument.id;
    const std::size_t trackCount = controller.project().tracks.size();
    const std::size_t undoDepth = controller.undoDepth();

    FakeSharedMutationSink sink;
    sink.result = result;
    controller.attachSharedMutationSink(sink);
    const std::string added =
        sampler ? controller.addPatternInstrument(pattern, *sampler, 0.0)
                : std::string{};
    controller.clearSamplerSample(loadedTrack, slotId);

    const auto* addBatch = sink.genericBodies.size() > 0
                               ? std::get_if<std::shared_ptr<
                                     daw::collab::BatchCommand>>(
                                     &sink.genericBodies[0])
                               : nullptr;
    const auto* remove = sink.genericBodies.size() > 1
                             ? std::get_if<
                                   daw::collab::RemovePluginAssetBinding>(
                                   &sink.genericBodies[1])
                             : nullptr;
    check(sink.genericCalls == 2 && addBatch && *addBatch &&
              !(*addBatch)->commands.empty() && remove &&
              remove->location.chain ==
                  daw::collab::PluginChain::Instrument &&
              remove->location.trackId == loadedTrack &&
              remove->insertId == slotId && remove->key == "sample",
          "empty Pattern Sampler and sample clear use typed shared commands");
    const daw::TrackModel* unchanged =
        controller.project().findTrack(loadedTrack);
    check((submitted ? !added.empty() : added.empty()) && unchanged &&
              unchanged->instrument.assetBindings.size() == 1 &&
              controller.project().tracks.size() == trackCount &&
              controller.undoDepth() == undoDepth,
          submitted
              ? "submitted Sampler commands avoid local mutation and legacy undo"
              : "blocked Sampler commands avoid local mutation and legacy undo");
    check(controller.detachSharedMutationSink(sink),
          "shared Sampler sink detaches");
}

void verifyProjectAndHistoryGates(
    daw::collab::SharedMutationResult result) {
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "project/history gate fixture initializes");
    controller.setProjectName("Before");
    const std::string track =
        controller.addTrack(daw::TrackKind::Audio, "Before");
    controller.renameTrack(track, "After");
    const std::size_t undoDepth = controller.undoDepth();

    FakeSharedMutationSink sink;
    sink.result = result;
    controller.attachSharedMutationSink(sink);
    controller.setProjectName("After");
    controller.undo();
    const auto* rename = sink.genericBodies.size() == 1
                             ? std::get_if<daw::collab::SetProjectScalar>(
                                   &sink.genericBodies.front())
                             : nullptr;
    check(rename && rename->field == daw::collab::ProjectScalar::Name &&
              std::get<std::string>(rename->value) == "After",
          "project name publishes one typed scalar command");
    check(controller.projectName() == "Before" &&
              controller.project().findTrack(track)->name == "After" &&
              controller.undoDepth() == undoDepth,
          "cloud project rename and legacy undo do not mutate the projection");

    check(controller.detachSharedMutationSink(sink),
          "project/history sink detaches for redo setup");
    controller.undo();
    check(controller.project().findTrack(track)->name == "Before" &&
              controller.canRedo(),
          "local undo prepares a legacy redo fixture");
    controller.attachSharedMutationSink(sink);
    const std::size_t depthBeforeRedo = controller.undoDepth();
    controller.redo();
    check(controller.project().findTrack(track)->name == "Before" &&
              controller.canRedo() &&
              controller.undoDepth() == depthBeforeRedo,
          "cloud-bound redo cannot enter the legacy closure stack");
    check(controller.detachSharedMutationSink(sink),
          "project/history sink detaches");
}

void verifyNonAssetExitGates(
    daw::collab::SharedMutationResult result) {
    const bool submitted =
        result == daw::collab::SharedMutationResult::Submitted;
    daw::ProjectModel project;
    daw::TrackModel audio;
    audio.id = daw::newUuid();
    audio.kind = daw::TrackKind::Audio;
    audio.name = "Audio";
    audio.inputEnabled = true;
    daw::ClipModel clip;
    clip.id = daw::newUuid();
    clip.kind = daw::ClipKind::Audio;
    clip.durationSeconds = 4.0;
    clip.musicalAnalysis.algorithmVersion = 1;
    clip.musicalAnalysis.analyzedDurationSeconds = 4.0;
    const std::string audioId = audio.id;
    const std::string clipId = clip.id;
    audio.clips.push_back(std::move(clip));
    daw::TrackModel midi;
    midi.id = daw::newUuid();
    midi.kind = daw::TrackKind::Midi;
    midi.name = "MIDI";
    const std::string midiId = midi.id;
    project.tracks.push_back(std::move(audio));
    project.tracks.push_back(std::move(midi));

    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk() &&
              controller.materializeCollaborationProject(std::move(project),
                                                         true)
                  .isOk(),
          "non-asset exit-gate fixture materializes");
    controller.setProjectName("Before");
    controller.setTempo(121.0);
    const std::size_t undoDepth = controller.undoDepth();

    FakeSharedMutationSink sink;
    sink.result = result;
    controller.attachSharedMutationSink(sink);

    daw::ClipMusicalAnalysisModel analysis;
    analysis.algorithmVersion = 7;
    analysis.tempo.status = daw::MusicalAnalysisStatus::Available;
    analysis.tempo.bpm = 120.0;
    analysis.tempo.confidence = 0.9;
    analysis.tempo.stability = 0.8;
    analysis.analyzedDurationSeconds = 4.0;
    const bool analysisResult = controller.setClipMusicalAnalysis(
        audioId, clipId, analysis, "Analyze");
    const auto* analysisCommand =
        sink.genericBodies.size() == 1
            ? std::get_if<daw::collab::SetClipMusicalAnalysis>(
                  &sink.genericBodies.front())
            : nullptr;
    check(analysisResult == submitted && analysisCommand &&
              analysisCommand->trackId == audioId &&
              analysisCommand->clipId == clipId &&
              analysisCommand->analysis.algorithmVersion == 7 &&
              controller.project()
                      .findTrack(audioId)
                      ->clips.front()
                      .musicalAnalysis.algorithmVersion == 1,
          "clip musical analysis uses one typed command without local mutation");

    const int callsBeforeBlockedApis = sink.genericCalls;
    controller.ensureInsertSlots(audioId, 4);
    controller.ensureMasterInsertSlots(4);
    controller.setTrackInstrument(midiId, "Legacy Instrument");
    daw::ProjectModel restored = controller.project();
    restored.name = "Restored";
    controller.restoreProject(restored, "Restore");
    controller.commitProjectGesture(restored, 0, "Gesture");
    const auto group = controller.beginUndoGroup();
    controller.collapseUndo(0, "Collapse");
    controller.collapseUndo(group, "Collapse Group");
    controller.releaseUndoGroup(group);
    check(sink.genericCalls == callsBeforeBlockedApis && !group &&
              controller.project().findTrack(audioId)->inserts.empty() &&
              controller.project().masterInserts.empty() &&
              controller.project().findTrack(midiId)->instrument.name.empty() &&
              controller.projectName() == "Before" &&
              controller.undoDepth() == undoDepth,
          "cloud project blocks legacy plugin slots, snapshots and undo groups");

    const int callsBeforeCountIn = sink.genericCalls;
    check(controller.armCountInExactly({audioId}, 2) &&
              controller.isCountingIn() && !controller.isRecording() &&
              sink.genericCalls == callsBeforeCountIn,
          "cloud count-in is an independent local transport action");
    controller.cancelCountIn();
    check(!controller.isCountingIn() && !controller.isRecording() &&
              sink.genericCalls == callsBeforeCountIn,
          "cancelling cloud count-in remains local");

    const int callsBeforePreview = sink.genericCalls;
    for (int index = 0; index < 100; ++index) {
        controller.setTrackVolumeLive(
            audioId, 0.5f + 0.005f * float(index));
    }
    const float previewVolume =
        controller.project().findTrack(audioId)->volume;
    check(sink.genericCalls == callsBeforePreview,
          "100 live volume previews publish zero durable commands");
    controller.commitTrackVolumeEdit({{audioId, 1.0f}}, "Set Volume");
    const auto* volumeBatch =
        sink.genericBodies.size() == std::size_t(callsBeforePreview + 1)
            ? std::get_if<std::shared_ptr<daw::collab::BatchCommand>>(
                  &sink.genericBodies.back())
            : nullptr;
    check(sink.genericCalls == callsBeforePreview + 1 && volumeBatch &&
              *volumeBatch && (*volumeBatch)->commands.size() == 1 &&
              std::fabs(controller.project().findTrack(audioId)->volume -
                        (submitted ? previewVolume : 1.0f)) < 1e-6f &&
              controller.undoDepth() == undoDepth,
          "volume release publishes one batch and blocked release rolls back");

    const int callsBeforeTrim = sink.genericCalls;
    controller.beginClipTrimEdit(audioId, clipId);
    controller.setClipTrim(audioId, clipId, 0.25, 0.25, 2.0);
    check(sink.genericCalls == callsBeforeTrim,
          "live clip trim publishes zero durable commands");
    controller.endClipTrimEdit("Trim Clip");
    const daw::ClipModel& trimmed =
        controller.project().findTrack(audioId)->clips.front();
    const auto* trimBatch =
        sink.genericBodies.size() == std::size_t(callsBeforeTrim + 1)
            ? std::get_if<std::shared_ptr<daw::collab::BatchCommand>>(
                  &sink.genericBodies.back())
            : nullptr;
    check(sink.genericCalls == callsBeforeTrim + 1 && trimBatch &&
              *trimBatch && !(*trimBatch)->commands.empty() &&
              std::fabs(trimmed.startSeconds -
                        (submitted ? 0.25 : 0.0)) < 1e-9 &&
              std::fabs(trimmed.offsetSeconds -
                        (submitted ? 0.25 : 0.0)) < 1e-9 &&
              std::fabs(trimmed.durationSeconds -
                        (submitted ? 2.0 : 4.0)) < 1e-9 &&
              controller.undoDepth() == undoDepth,
          "trim release publishes one batch and blocked release rolls back");

    check(controller.detachSharedMutationSink(sink),
          "non-asset exit-gate sink detaches");
}

void verifyCompExitGates(daw::collab::SharedMutationResult result) {
    const bool submitted = result == daw::collab::SharedMutationResult::Submitted;
    daw::ProjectModel project;
    daw::TrackModel track;
    track.id = daw::newUuid();
    track.kind = daw::TrackKind::Audio;
    daw::ClipModel clip;
    clip.id = daw::newUuid();
    clip.kind = daw::ClipKind::Audio;
    clip.durationSeconds = 1.0;
    daw::TakeModel used;
    used.id = daw::newUuid();
    used.name = "Used";
    used.lengthSeconds = 1.0;
    daw::TakeModel unused = used;
    unused.id = daw::newUuid();
    unused.name = "Unused";
    clip.takes = {used, unused};
    daw::CompSegment segment;
    segment.id = daw::newUuid();
    segment.takeId = used.id;
    segment.endSeconds = 1.0;
    clip.comp.push_back(segment);
    const std::string trackId = track.id;
    const std::string clipId = clip.id;
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));

    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk() &&
              controller.materializeCollaborationProject(std::move(project),
                                                         true)
                  .isOk(),
          "comp exit-gate fixture materializes");
    const std::size_t undoDepth = controller.undoDepth();
    FakeSharedMutationSink sink;
    sink.result = result;
    controller.attachSharedMutationSink(sink);
    controller.beginCompEdit(trackId, clipId);
    controller.setCompSegment(trackId, clipId, unused.id, 0.0, 0.5);
    const bool unchangedDuringGesture =
        controller.project().findTrack(trackId)->clips.front().comp.size() ==
            1 &&
        controller.project()
                .findTrack(trackId)
                ->clips.front()
                .comp.front()
                .takeId == used.id;
    controller.endCompEdit();
    controller.selectTake(trackId, clipId, unused.id);
    controller.commitComp(trackId, clipId);
    const std::size_t cropped = controller.cropToComp(trackId, clipId);
    const std::size_t removed = controller.deleteUnusedTakes(true);

    const auto* batch = sink.genericBodies.size() == 3
                            ? std::get_if<std::shared_ptr<
                                  daw::collab::BatchCommand>>(
                                  &sink.genericBodies.back())
                            : nullptr;
    const auto* unchanged = controller.project().findTrack(trackId);
    const bool routed = unchangedDuringGesture && sink.genericCalls == 3 &&
                        allBatches(sink, 0) && batch && *batch &&
                        (*batch)->commands.size() == 1 && cropped == 0 &&
                        removed == (submitted ? 1u : 0u);
    check(routed,
          "comp draft/select/delete submit batches while commit/crop stay blocked");
    check(unchanged && unchanged->clips.front().takes.size() == 2 &&
              unchanged->clips.front().comp.size() == 1 &&
              controller.undoDepth() == undoDepth,
          submitted
              ? "submitted comp gates avoid local mutation and legacy undo"
              : "blocked comp gates avoid local mutation and legacy undo");
    check(controller.detachSharedMutationSink(sink),
          "comp exit-gate sink detaches");
}

void verifyTemplateBatch(daw::collab::SharedMutationResult result) {
    const bool submitted = result == daw::collab::SharedMutationResult::Submitted;
    const auto package = std::filesystem::temp_directory_path() /
                         ("vlt-shared-template-" + daw::newUuid() + ".vltt");
    daw::EngineController source;
    source.initialize(48000.0, 512, false);
    const std::string folder = source.addFolder(false, "Folder");
    const std::string child =
        source.addTrack(daw::TrackKind::Audio, "Child");
    source.moveTrackToFolder(child, folder);
    check(source.saveProjectTemplate(package.string(), "Template").isOk(),
          "shared template fixture saves");

    daw::EngineController destination;
    destination.initialize(48000.0, 512, false);
    destination.addTrack(daw::TrackKind::Audio, "Existing");
    const std::size_t tracksBefore = destination.project().tracks.size();
    const std::size_t undoDepth = destination.undoDepth();
    FakeSharedMutationSink sink;
    sink.result = result;
    destination.attachSharedMutationSink(sink);
    std::vector<std::string> imported;
    const audio::Result importedResult =
        destination.importProjectTemplateTracks(package.string(), imported);
    check(sink.genericCalls == 1 && allBatches(sink, 0) &&
              bool(importedResult) == submitted &&
              (submitted ? imported.size() == 2 : imported.empty()),
          "template import submits one scratch-validated outer batch");
    check(destination.project().tracks.size() == tracksBefore &&
              destination.undoDepth() == undoDepth,
          submitted
              ? "submitted template import avoids local mutation and legacy undo"
              : "blocked template import avoids local mutation and legacy undo");
    check(destination.detachSharedMutationSink(sink),
          "shared template sink detaches");
    std::error_code ignored;
    std::filesystem::remove_all(package, ignored);
}

void verifyScratchBatchMutators(daw::collab::SharedMutationResult result) {
    const bool submitted = result == daw::collab::SharedMutationResult::Submitted;
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "scratch batch fixture initializes");
    const std::string source =
        controller.addTrack(daw::TrackKind::Audio, "Source");
    const std::string target =
        controller.addTrack(daw::TrackKind::Audio, "Target");
    const std::string bus =
        controller.addTrack(daw::TrackKind::Bus, "Bus");
    const std::string send = controller.addSend(source, bus);
    const std::string midi =
        controller.addTrack(daw::TrackKind::Midi, "MIDI");
    const std::string clip = controller.addMidiClip(midi, 0.0, 1.0);
    const std::string pattern = controller.addPattern("Pattern");
    const double patternDuration =
        controller.project().findTrack(pattern)->clips.front().durationSeconds;
    const std::string patternChild =
        controller.addTrack(daw::TrackKind::Midi, "Pattern Child");
    controller.moveTrackToFolder(patternChild, pattern);
    const std::string patternMember =
        controller.addMidiClip(patternChild, 0.0, patternDuration);
    check(!source.empty() && !target.empty() && !send.empty() &&
              !clip.empty() && !pattern.empty() && !patternMember.empty() &&
              controller.project().findTrack(patternChild)->clips.front()
                      .patternClipId ==
                  controller.project().findTrack(pattern)->clips.front().id,
          "scratch batch fixture has stable entities");
    const std::size_t trackCount = controller.project().tracks.size();
    const std::size_t midiClipCount =
        controller.project().findTrack(midi)->clips.size();
    const std::size_t targetSendCount =
        controller.project().findTrack(target)->sends.size();
    const std::size_t undoDepth = controller.undoDepth();
    const std::string patternClip =
        controller.project().findTrack(pattern)->clips.front().id;
    const double patternSplitAt =
        controller.project().findTrack(pattern)->clips.front().durationSeconds /
        2.0;

    FakeSharedMutationSink sink;
    sink.result = result;
    controller.attachSharedMutationSink(sink);
    const std::string trackCopy = controller.duplicateTrack(source, true);
    const int afterTrack = sink.genericCalls;
    const std::string patternCopy = controller.duplicatePattern(pattern);
    const int afterPattern = sink.genericCalls;
    const std::string folder =
        controller.packIntoFolder({source, target}, "Group", true);
    const int afterFolder = sink.genericCalls;
    const std::string clipCopy =
        controller.duplicateClipAt(midi, clip, 2.0);
    const int afterClipAt = sink.genericCalls;
    const std::string clipRepeat = controller.duplicateClip(midi, clip);
    const int afterClip = sink.genericCalls;
    const std::string split = controller.splitClip(midi, clip, 0.5);
    const int afterSplit = sink.genericCalls;
    const std::string patternSplit =
        controller.splitClip(pattern, patternClip, patternSplitAt);
    const int afterPatternSplit = sink.genericCalls;
    const bool sendsCopied = controller.copySendsTo(source, target, false);

    const bool reachedOnce =
        afterTrack == 1 && afterPattern == 2 && afterFolder == 3 &&
        afterClipAt == 4 && afterClip == 5 && afterSplit == 6 &&
        afterPatternSplit == 7 && sink.genericCalls == 8;
    if (!reachedOnce) {
        std::printf("counts: track=%d pattern=%d folder=%d clipAt=%d clip=%d split=%d patternSplit=%d sends=%d\n",
                    afterTrack, afterPattern, afterFolder, afterClipAt,
                    afterClip, afterSplit, afterPatternSplit,
                    sink.genericCalls);
    }
    check(reachedOnce, "every complex mutator reaches the sink exactly once");

    check(sink.genericCalls == 8 && allBatches(sink, 0),
          "each scratch-planned gesture submits one non-nested outer batch");
    check((submitted ? !trackCopy.empty() : trackCopy.empty()) &&
              (submitted ? !patternCopy.empty() : patternCopy.empty()) &&
              (submitted ? !folder.empty() : folder.empty()) &&
              (submitted ? !clipCopy.empty() : clipCopy.empty()) &&
              (submitted ? !clipRepeat.empty() : clipRepeat.empty()) &&
              (submitted ? !split.empty() : split.empty()) &&
              (submitted ? !patternSplit.empty() : patternSplit.empty()) &&
              sendsCopied == submitted,
          submitted ? "submitted batch results reach callers"
                    : "blocked batch results reach callers");
    check(controller.project().tracks.size() == trackCount &&
              controller.project().findTrack(midi)->clips.size() ==
                  midiClipCount &&
              controller.project().findTrack(target)->sends.size() ==
                  targetSendCount &&
              controller.undoDepth() == undoDepth,
          submitted
              ? "submitted scratch batches avoid local mutation and legacy undo"
              : "blocked scratch batches avoid local mutation and legacy undo");
    check(controller.detachSharedMutationSink(sink),
          "scratch batch sink detaches");
}

daw::InsertModel sharedEqualizer() {
    daw::InsertModel insert;
    insert.id = daw::newUuid();
    insert.name = "VLT Equalizer";
    insert.format = daw::PluginFormat::Internal;
    insert.uid = "daw.equalizer";
    insert.vendor = "VLT";
    insert.pluginVersion = "1.0";
    insert.stateSchemaVersion = 1;
    return insert;
}

void verifySharedChannelBatchMutators(
    daw::collab::SharedMutationResult result) {
    const bool submitted = result == daw::collab::SharedMutationResult::Submitted;
    daw::ProjectModel project;
    daw::TrackModel source;
    source.id = daw::newUuid();
    source.kind = daw::TrackKind::Audio;
    source.name = "Source";
    source.inserts.push_back(sharedEqualizer());
    daw::TrackModel target;
    target.id = daw::newUuid();
    target.kind = daw::TrackKind::Audio;
    target.name = "Target";
    daw::TrackModel bus;
    bus.id = daw::newUuid();
    bus.kind = daw::TrackKind::Bus;
    bus.name = "Bus";
    daw::SendModel send;
    send.id = daw::newUuid();
    send.destinationTrackId = bus.id;
    source.sends.push_back(send);
    const std::string sourceId = source.id;
    const std::string targetId = target.id;
    const std::string insertId = source.inserts.front().id;
    project.tracks.push_back(std::move(source));
    project.tracks.push_back(std::move(target));
    project.tracks.push_back(std::move(bus));

    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk() &&
              controller.materializeCollaborationProject(std::move(project), true)
                  .isOk(),
          "shared channel fixture materializes");
    const auto plugins = controller.copyChannelStrip(sourceId, false);
    const auto strip = controller.copyChannelStrip(sourceId, true);
    auto mixPreset = strip;
    mixPreset.inserts.clear();
    const auto presetPath = std::filesystem::temp_directory_path() /
                            ("vlt-shared-preset-" + daw::newUuid() + ".vlts");
    check(daw::ChannelStripPreset::save(mixPreset, presetPath.string()).isOk(),
          "shared channel fixture writes a mix-only preset");
    const std::size_t undoDepth = controller.undoDepth();

    FakeSharedMutationSink sink;
    sink.result = result;
    controller.attachSharedMutationSink(sink);
    const bool pasted = controller.pasteChannelInserts(targetId, plugins);
    const bool stripPasted = controller.pasteChannelStrip(targetId, strip);
    const bool presetPasted =
        controller.pasteChannelStripPreset(targetId, strip);
    const bool filePresetApplied =
        controller.applyChannelStripPreset(targetId, presetPath.string()).isOk();
    const bool moved = controller.moveInsertBetweenChannels(
        sourceId, insertId, targetId, 0, false);

    check(sink.genericCalls == 5 && allBatches(sink, 0),
          "channel paste/move gestures each submit one outer batch");
    check(pasted == submitted && stripPasted == submitted &&
              presetPasted == submitted && filePresetApplied == submitted &&
              moved == submitted,
          submitted ? "submitted channel batch results reach callers"
                    : "blocked channel batch results reach callers");
    const auto* unchangedSource = controller.project().findTrack(sourceId);
    const auto* unchangedTarget = controller.project().findTrack(targetId);
    check(unchangedSource && unchangedSource->inserts.size() == 1 &&
              unchangedTarget && unchangedTarget->inserts.empty() &&
              unchangedTarget->sends.empty() &&
              controller.undoDepth() == undoDepth,
          submitted
              ? "submitted channel batches avoid local mutation and legacy undo"
              : "blocked channel batches avoid local mutation and legacy undo");
    check(controller.detachSharedMutationSink(sink),
          "shared channel batch sink detaches");
    std::error_code ignored;
    std::filesystem::remove(presetPath, ignored);
}

void verifyCapabilityLedger() {
    using daw::collab::MutationCapability;
    std::unordered_set<std::string_view> names;
    bool shared = false;
    bool local = false;
    bool blocked = false;
    bool classified = true;
    for (const auto& entry : daw::collab::kMutationCapabilityLedger) {
        classified &= !entry.method.empty() &&
                      entry.capability != MutationCapability::Unclassified;
        shared |= entry.capability == MutationCapability::SharedCommand;
        local |= entry.capability == MutationCapability::LocalOnly;
        blocked |= entry.capability == MutationCapability::BlockedV1;
        classified &= names.insert(entry.method).second;
    }
    check(classified && shared && local && blocked && names.size() >= 100,
          "cloud mutation ledger is complete, unique and fully classified");
    const auto capabilityOf = [](std::string_view method) {
        for (const auto& entry : daw::collab::kMutationCapabilityLedger) {
            if (entry.method == method) return entry.capability;
        }
        return MutationCapability::Unclassified;
    };
    check(capabilityOf("addPatternInstrument") ==
                  MutationCapability::SharedCommand &&
              capabilityOf("clearSamplerSample") ==
                  MutationCapability::SharedCommand &&
              capabilityOf("setProjectName") ==
                  MutationCapability::SharedCommand &&
              capabilityOf("importProjectTemplateTracks") ==
                  MutationCapability::SharedCommand &&
              capabilityOf("deleteUnusedTakes") ==
                  MutationCapability::SharedCommand &&
              capabilityOf("setClipMusicalAnalysis") ==
                  MutationCapability::SharedCommand &&
              capabilityOf("setTrackVolumeLive") ==
                  MutationCapability::LocalOnly &&
              capabilityOf("setLanePoints") ==
                  MutationCapability::LocalOnly &&
              capabilityOf("commitTrackVolumeEdit") ==
                  MutationCapability::SharedCommand &&
              capabilityOf("commitLaneEdit") ==
                  MutationCapability::SharedCommand &&
              capabilityOf("endClipTrimEdit") ==
                  MutationCapability::SharedCommand &&
              capabilityOf("setTrackInstrument") ==
                  MutationCapability::BlockedV1 &&
              capabilityOf("ensureInsertSlots") ==
                  MutationCapability::BlockedV1 &&
              capabilityOf("restoreProject") ==
                  MutationCapability::BlockedV1 &&
              capabilityOf("commitProjectGesture") ==
                  MutationCapability::BlockedV1 &&
              capabilityOf("beginUndoGroup") ==
                  MutationCapability::BlockedV1 &&
              capabilityOf("startRecording") ==
                  MutationCapability::LocalOnly &&
              capabilityOf("armCountIn") ==
                  MutationCapability::LocalOnly &&
              capabilityOf("commitComp") == MutationCapability::BlockedV1 &&
              capabilityOf("cropToComp") == MutationCapability::BlockedV1 &&
              capabilityOf("undo") == MutationCapability::BlockedV1 &&
              capabilityOf("redo") == MutationCapability::BlockedV1,
          "Sampler, project, template, take and legacy-history gates are classified");
}

void verifyConsumedMutation(daw::collab::SharedMutationResult result) {
    const bool submitted =
        result == daw::collab::SharedMutationResult::Submitted;
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          submitted ? "submitted fixture initializes"
                    : "blocked fixture initializes");
    controller.setProjectKey(2, "minor");
    controller.setAiInstructions("before");
    const std::string trackId =
        controller.addTrack(daw::TrackKind::Audio, "Before");
    controller.setTrackMuted(trackId, true);
    const std::size_t undoDepth = controller.undoDepth();

    FakeSharedMutationSink sink;
    sink.result = result;
    controller.attachSharedMutationSink(sink);
    check(controller.sharedMutationSink() == &sink,
          submitted ? "submitted sink attaches" : "blocked sink attaches");

    check(controller.setTimeSignature(7, 8) == result &&
              controller.setProjectKey(-1, "dorian") == result &&
              controller.setAiInstructions("after") == result &&
              controller.renameTrack(trackId, "After") == result &&
              controller.setTrackMuted(trackId, false) == result &&
              controller.clearAllMutes() == result,
          submitted ? "submitted outcomes reach the initiating UI"
                    : "blocked outcomes reach the initiating UI");

    check(sink.timeSignatureCalls == 1 && sink.numerator == 7 &&
              sink.denominator == 8,
          submitted ? "submitted time-signature payload is exact"
                    : "blocked time-signature payload is exact");
    check(sink.projectKeyCalls == 1 && sink.keyRoot == 11 &&
              sink.scale == "dorian",
          submitted ? "submitted project-key payload is canonical"
                    : "blocked project-key payload is canonical");
    check(sink.aiInstructionsCalls == 1 && sink.aiInstructions == "after",
          submitted ? "submitted AI-instructions payload is exact"
                    : "blocked AI-instructions payload is exact");
    check(sink.renameTrackCalls == 1 && sink.renamedTrackId == trackId &&
              sink.trackName == "After",
          submitted ? "submitted rename payload is exact"
                    : "blocked rename payload is exact");
    check(sink.trackMutedCalls == 1 && sink.mutedTrackId == trackId &&
              !sink.muted,
          submitted ? "submitted mute payload is exact"
                    : "blocked mute payload is exact");
    check(sink.clearAllMutesCalls == 1 &&
              sink.clearedTrackIds == std::vector<std::string>{trackId},
          submitted ? "submitted clear-mutes batch is exact"
                    : "blocked clear-mutes batch is exact");

    const auto* track = controller.project().findTrack(trackId);
    check(controller.timeSigNumerator() == 4 &&
              controller.timeSigDenominator() == 4 &&
              controller.keyRoot() == 2 &&
              controller.projectScale() == "minor" &&
              controller.aiInstructions() == "before" && track &&
              track->name == "Before" && track->muted,
          submitted ? "submitted commands never mutate the local document"
                    : "blocked commands never mutate the local document");
    check(controller.undoDepth() == undoDepth,
          submitted ? "submitted commands never enter legacy undo"
                    : "blocked commands never enter legacy undo");

    {
        FakeSharedMutationSink replacement;
        controller.attachSharedMutationSink(replacement);
        check(!controller.detachSharedMutationSink(sink) &&
                  controller.sharedMutationSink() == &replacement,
              "stale sink cannot detach its replacement");
        check(controller.detachSharedMutationSink(replacement) &&
                  controller.sharedMutationSink() == nullptr,
              "active sink detaches explicitly");
    }
    controller.setAiInstructions("after detach");
    check(controller.aiInstructions() == "after detach",
          "local mutation is safe after the detached sink is destroyed");
}

void verifyAtomicMuteGesture(daw::collab::SharedMutationResult result) {
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "atomic-mute fixture initializes");
    const std::string folder = controller.addFolder(false, "Folder");
    const std::string child =
        controller.addTrack(daw::TrackKind::Audio, "Child");
    const std::string peer =
        controller.addTrack(daw::TrackKind::Audio, "Peer");
    controller.moveTrackToFolder(child, folder);
    const std::size_t undoDepth = controller.undoDepth();

    FakeSharedMutationSink sink;
    sink.result = result;
    sink.cloudBinding =
        result != daw::collab::SharedMutationResult::LocalFallback;
    controller.attachSharedMutationSink(sink);
    const std::array<std::string, 4> selected{folder, child, folder, peer};
    const auto outcome = controller.setTracksMuted(selected, true);

    check(outcome == result && sink.tracksMutedCalls == 1 &&
              sink.tracksMuted &&
              sink.mutedTrackIds ==
                  std::vector<std::string>({folder, child, peer}),
          "multi/folder mute expands descendants and deduplicates once");
    const bool locallyApplied =
        result == daw::collab::SharedMutationResult::LocalFallback;
    check(controller.project().findTrack(folder)->muted == locallyApplied &&
              controller.project().findTrack(child)->muted == locallyApplied &&
              controller.project().findTrack(peer)->muted == locallyApplied &&
              controller.undoDepth() == undoDepth,
          locallyApplied
              ? "atomic fallback preserves local mute without legacy undo"
              : "submitted/blocked atomic mute avoids local mutation and undo");
    check(controller.detachSharedMutationSink(sink),
          "atomic-mute sink detaches before its lifetime ends");
}

// Track creation has to leave the same document behind whether or not a cloud
// session is bound. Two ways it used not to, both regressions worth pinning.
void verifyTrackCreationParity() {
    {
        // A Pattern sums by definition, so isSummingFolder() reports true for
        // it regardless of the field. Writing the field anyway made the local
        // path disagree with the cloud path, where AddTrack cannot carry it —
        // and the reducer refuses SetTrackProperty::Summing for non-folders,
        // so "just send the command" would fail the whole batch.
        daw::EngineController controller;
        check(controller.initialize(48000.0, 512, false).isOk(),
              "pattern parity fixture initializes");
        const std::string pattern = controller.addPattern("Pattern");
        const daw::TrackModel* local = controller.project().findTrack(pattern);
        check(local && !local->summing && daw::isSummingFolder(*local),
              "a local Pattern sums without carrying the summing field");
    }

    {
        daw::EngineController controller;
        check(controller.initialize(48000.0, 512, false).isOk(),
              "shared pattern fixture initializes");
        FakeSharedMutationSink sink;
        controller.attachSharedMutationSink(sink);
        const std::string pattern = controller.addPattern("Pattern");
        const bool sharedOnce = !pattern.empty() && sink.genericCalls == 1;
        check(sharedOnce &&
                  std::holds_alternative<
                      std::shared_ptr<daw::collab::BatchCommand>>(
                      sink.genericBodies.back()) &&
                  !commandContainsString(sink.genericBodies.back(), "summing"),
              "a shared Pattern never sends a summing property the reducer "
              "would reject");
        controller.detachSharedMutationSink(sink);
    }

    {
        // isFolder() also admits a Pattern, so setFolderSumming used to reach
        // one and submit a command the reducer rejects.
        daw::EngineController controller;
        check(controller.initialize(48000.0, 512, false).isOk(),
              "pattern summing gate fixture initializes");
        const std::string pattern = controller.addPattern("Pattern");
        FakeSharedMutationSink sink;
        controller.attachSharedMutationSink(sink);
        controller.setFolderSumming(pattern, false);
        controller.setFolderSumming(pattern, true);
        check(sink.genericCalls == 0,
              "setFolderSumming refuses a Pattern instead of sending a "
              "doomed command");
        controller.detachSharedMutationSink(sink);
    }

    {
        // automationExpanded is LocalOnly, so no command carries it and each
        // participant has to set it for themselves. The cloud branch used to
        // return before doing so, hiding the new lane under a collapsed parent.
        daw::EngineController controller;
        check(controller.initialize(48000.0, 512, false).isOk(),
              "automation lane fixture initializes");
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Automated");
        controller.setAutomationExpanded(track, false);
        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackVolume;
        target.channelId = track;

        FakeSharedMutationSink sink;
        controller.attachSharedMutationSink(sink);
        const std::string lane = controller.addAutomationLane(track, target);
        const daw::TrackModel* owner = controller.project().findTrack(track);
        check(!lane.empty() && sink.genericCalls == 1 && owner &&
                  owner->automationExpanded,
              "a shared automation lane expands its parent locally");
        controller.detachSharedMutationSink(sink);
    }

    {
        // The same call must not expand anything when the session refused it.
        daw::EngineController controller;
        check(controller.initialize(48000.0, 512, false).isOk(),
              "blocked automation lane fixture initializes");
        const std::string track =
            controller.addTrack(daw::TrackKind::Audio, "Automated");
        controller.setAutomationExpanded(track, false);
        daw::AutomationTarget target;
        target.kind = daw::AutomationTargetKind::TrackVolume;
        target.channelId = track;

        FakeSharedMutationSink sink;
        sink.result = daw::collab::SharedMutationResult::Blocked;
        controller.attachSharedMutationSink(sink);
        const std::string lane = controller.addAutomationLane(track, target);
        const daw::TrackModel* owner = controller.project().findTrack(track);
        check(lane.empty() && owner && !owner->automationExpanded,
              "a blocked automation lane leaves the parent untouched");
        controller.detachSharedMutationSink(sink);
    }
}

/// Reproduces what the real gateway does and the fake sinks above do not: a
/// submit projects optimistically and hands back a rebuilt document on the same
/// stack, replacing m_project.tracks wholesale before the mutator that called
/// submit has returned. Every TrackModel*/ClipModel* into the old document dies
/// at that moment. Running the cloud branches through this under ASan is what
/// turns "nobody holds a pointer across a submit" from a review promise into a
/// checked property.
class ReprojectingSharedMutationSink final
    : public daw::collab::SharedMutationSink {
public:
    daw::EngineController* owner = nullptr;
    int submits = 0;

    bool handlesCloudBinding() override { return true; }

    daw::collab::SharedMutationResult submit(
        daw::collab::SharedMutationRequest) override {
        ++submits;
        reproject();
        return daw::collab::SharedMutationResult::Submitted;
    }

    daw::collab::SharedMutationResult setTimeSignature(int, int) override {
        return reprojected();
    }
    daw::collab::SharedMutationResult setProjectKey(int,
                                                    std::string_view) override {
        return reprojected();
    }
    daw::collab::SharedMutationResult setAiInstructions(
        std::string_view) override {
        return reprojected();
    }
    daw::collab::SharedMutationResult renameTrack(std::string_view,
                                                  std::string_view) override {
        return reprojected();
    }
    daw::collab::SharedMutationResult setTrackMuted(std::string_view,
                                                    bool) override {
        return reprojected();
    }
    daw::collab::SharedMutationResult setTracksMuted(
        std::span<const std::string>, bool) override {
        return reprojected();
    }
    daw::collab::SharedMutationResult clearAllMutes(
        std::span<const std::string>) override {
        return reprojected();
    }

private:
    daw::collab::SharedMutationResult reprojected() {
        ++submits;
        reproject();
        return daw::collab::SharedMutationResult::Submitted;
    }

    // A byte-identical document still reallocates the vectors, which is the
    // whole point: identity of the data is not identity of the storage.
    void reproject() {
        if (!owner) return;
        daw::ProjectModel next = owner->project();
        (void)owner->materializeCollaborationProject(std::move(next), false);
    }
};

// Drive the cloud branches of the mutators most likely to hold a raw pointer
// into m_project across their submit. The assertions are deliberately weak —
// the point is that the calls survive the document being swapped underneath
// them, which ASan/UBSan turn into a hard failure.
void verifyPointerSafetyAcrossSubmit() {
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "pointer safety fixture initializes");

    const std::string audio =
        controller.addTrack(daw::TrackKind::Audio, "Audio");
    const std::string midi = controller.addTrack(daw::TrackKind::Midi, "MIDI");
    const std::string bus = controller.addTrack(daw::TrackKind::Bus, "Bus");
    const std::string clip = controller.addMidiClip(midi, 0.0, 4.0);
    const std::string note = controller.addNote(midi, clip, 60, 0.0, 1.0, 100);
    const std::string pattern = controller.addPattern("Pattern");
    const std::string child =
        controller.addTrack(daw::TrackKind::Midi, "Pattern Child");
    controller.moveTrackToFolder(child, pattern);
    const std::string send = controller.addSend(audio, bus);
    const std::string folder = controller.addFolder(true, "Group");
    check(!audio.empty() && !midi.empty() && !clip.empty() && !note.empty() &&
              !pattern.empty() && !send.empty() && !folder.empty(),
          "pointer safety fixture has stable entities");

    ReprojectingSharedMutationSink sink;
    sink.owner = &controller;
    controller.attachSharedMutationSink(sink);

    // Each of these takes a different route into m_project before submitting.
    controller.renameTrack(midi, "Renamed");
    controller.setTrackVolume(audio, 0.5f);
    controller.setTrackPan(audio, -0.25f);
    controller.setTrackMuted(audio, true);
    controller.setTrackColor(audio, 0x223344);
    controller.setTrackOutputBus(audio, bus);
    controller.setSendLevel(audio, send, 0.5f);
    controller.commitSendLevelEdit(audio, send, 0.25f);
    controller.setSendPreFader(audio, send, true);
    controller.setSendEnabled(audio, send, false);
    controller.setNoteVelocity(midi, clip, note, 90);
    controller.setNoteMuted(midi, clip, note, true);
    controller.setNote(midi, clip, note, 62, 0.5, 1.5);
    controller.setClipStartSeconds(midi, clip, 1.0);
    controller.setClipName(midi, clip, "Renamed clip");
    controller.setClipMuted(midi, clip, true);
    controller.moveTrackToFolder(audio, folder);
    controller.setFolderSumming(folder, false);
    controller.addPatternClip(pattern, 4.0, 2.0);
    controller.duplicateClip(midi, clip);
    controller.splitClip(midi, clip, 2.0);
    controller.addMidiClip(midi, 8.0, 2.0);
    controller.addTrack(daw::TrackKind::Audio, "Late");
    controller.removeTrack(bus);

    check(sink.submits > 0 &&
              controller.project().findTrack(midi) != nullptr,
          "cloud mutators survive the document being reprojected mid-submit");
    controller.detachSharedMutationSink(sink);
}

void verifyLocalFallback() {
    daw::EngineController controller;
    check(controller.initialize(48000.0, 512, false).isOk(),
          "fallback fixture initializes");
    const std::string first =
        controller.addTrack(daw::TrackKind::Audio, "First");
    const std::string second =
        controller.addTrack(daw::TrackKind::Audio, "Second");

    FakeSharedMutationSink sink;
    sink.result = daw::collab::SharedMutationResult::LocalFallback;
    sink.cloudBinding = false;
    controller.attachSharedMutationSink(sink);

    std::size_t undoDepth = controller.undoDepth();
    controller.setTimeSignature(7, 8);
    check(controller.timeSigNumerator() == 7 &&
              controller.timeSigDenominator() == 8 &&
              controller.undoDepth() == undoDepth + 1,
          "time signature keeps local mutation and undo on fallback");
    controller.undo();
    check(controller.timeSigNumerator() == 4 &&
              controller.timeSigDenominator() == 4,
          "fallback time-signature undo restores the old value");
    controller.redo();
    check(controller.timeSigNumerator() == 7 &&
              controller.timeSigDenominator() == 8,
          "fallback time-signature redo restores the new value");

    undoDepth = controller.undoDepth();
    controller.setProjectKey(-1, "dorian");
    check(controller.keyRoot() == 11 && controller.projectScale() == "dorian" &&
              controller.undoDepth() == undoDepth + 1 && sink.keyRoot == 11,
          "project key keeps canonical local value and sink payload");
    controller.undo();
    check(controller.keyRoot() == 0 && controller.projectScale() == "major",
          "fallback project-key undo restores the old value");
    controller.redo();
    check(controller.keyRoot() == 11 && controller.projectScale() == "dorian",
          "fallback project-key redo restores the new value");

    undoDepth = controller.undoDepth();
    controller.setAiInstructions("mix quietly");
    check(controller.aiInstructions() == "mix quietly" &&
              controller.undoDepth() == undoDepth,
          "AI instructions keep their non-undoable local fallback semantics");
    const int aiCalls = sink.aiInstructionsCalls;
    controller.setAiInstructions("mix quietly");
    check(sink.aiInstructionsCalls == aiCalls,
          "unchanged AI instructions do not reach the command sink");

    undoDepth = controller.undoDepth();
    controller.renameTrack(first, "Lead");
    check(controller.project().findTrack(first)->name == "Lead" &&
              controller.undoDepth() == undoDepth + 1,
          "track rename keeps local mutation and undo on fallback");
    controller.undo();
    check(controller.project().findTrack(first)->name == "First",
          "fallback rename undo restores the old name");
    controller.redo();
    check(controller.project().findTrack(first)->name == "Lead",
          "fallback rename redo restores the new name");

    undoDepth = controller.undoDepth();
    controller.setTrackMuted(first, true);
    controller.setTrackMuted(second, true);
    check(controller.project().findTrack(first)->muted &&
              controller.project().findTrack(second)->muted &&
              controller.undoDepth() == undoDepth,
          "track mute keeps its non-undoable local fallback semantics");
    const int muteCalls = sink.trackMutedCalls;
    controller.setTrackMuted(first, true);
    check(sink.trackMutedCalls == muteCalls,
          "unchanged track mute does not reach the command sink");
    controller.clearAllMutes();
    check(!controller.project().findTrack(first)->muted &&
              !controller.project().findTrack(second)->muted &&
              controller.undoDepth() == undoDepth &&
              sink.clearAllMutesCalls == 1 &&
              sink.clearedTrackIds ==
                  std::vector<std::string>({first, second}),
          "clear-all-mutes falls back atomically without legacy undo");

    check(controller.detachSharedMutationSink(sink),
          "fallback sink detaches before its lifetime ends");
}

} // namespace

int main() {
    verifyCapabilityLedger();
    verifySharedAssetMutationGate();
    verifyVerifiedAssetActions();
    check(daw::collab::marksLocalFileDirty(
              daw::collab::SharedMutationResult::LocalFallback) &&
              !daw::collab::marksLocalFileDirty(
                  daw::collab::SharedMutationResult::Submitted) &&
              !daw::collab::marksLocalFileDirty(
                  daw::collab::SharedMutationResult::Blocked),
          "only LocalFallback enters legacy file-dirty handling");
    verifyConsumedMutation(daw::collab::SharedMutationResult::Submitted);
    verifyConsumedMutation(daw::collab::SharedMutationResult::Blocked);
    verifyGenericMutationRoutes(daw::collab::SharedMutationResult::Submitted);
    verifyGenericMutationRoutes(daw::collab::SharedMutationResult::Blocked);
    verifyTakeMoveRoutes(daw::collab::SharedMutationResult::Submitted);
    verifyTakeMoveRoutes(daw::collab::SharedMutationResult::Blocked);
    verifySamplerBatchMutators(
        daw::collab::SharedMutationResult::Submitted);
    verifySamplerBatchMutators(daw::collab::SharedMutationResult::Blocked);
    verifyProjectAndHistoryGates(
        daw::collab::SharedMutationResult::Submitted);
    verifyProjectAndHistoryGates(
        daw::collab::SharedMutationResult::Blocked);
    verifyNonAssetExitGates(daw::collab::SharedMutationResult::Submitted);
    verifyNonAssetExitGates(daw::collab::SharedMutationResult::Blocked);
    verifyCompExitGates(daw::collab::SharedMutationResult::Submitted);
    verifyCompExitGates(daw::collab::SharedMutationResult::Blocked);
    verifyTemplateBatch(daw::collab::SharedMutationResult::Submitted);
    verifyTemplateBatch(daw::collab::SharedMutationResult::Blocked);
    verifyScratchBatchMutators(daw::collab::SharedMutationResult::Submitted);
    verifyScratchBatchMutators(daw::collab::SharedMutationResult::Blocked);
    verifySharedChannelBatchMutators(
        daw::collab::SharedMutationResult::Submitted);
    verifySharedChannelBatchMutators(
        daw::collab::SharedMutationResult::Blocked);
    verifyTrackCreationParity();
    verifyPointerSafetyAcrossSubmit();
    verifyLocalFallback();
    verifyAtomicMuteGesture(daw::collab::SharedMutationResult::Submitted);
    verifyAtomicMuteGesture(daw::collab::SharedMutationResult::Blocked);
    verifyAtomicMuteGesture(daw::collab::SharedMutationResult::LocalFallback);

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures;
}
