#include "cloud/CloudDocumentProjection.hpp"
#include "cloud/CloudPublicationCapture.hpp"
#include "cloud/PublishPreflight.hpp"
#include "collaboration/CollaborationState.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Core/AudioBuffer.hpp"
#include "Recording/RecordingEngine.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

int failures = 0;
namespace fs = std::filesystem;

bool check(bool condition, const char* label) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
    return condition;
}

daw::AssetRef asset(std::string id, daw::AssetKind kind,
                    std::string originalName = {}) {
    daw::AssetRef result;
    result.assetId = std::move(id);
    result.sha256.assign(64, 'a');
    result.kind = kind;
    result.byteSize = 4096;
    result.originalName = std::move(originalName);
    return result;
}

daw::InsertModel builtin(std::string id, std::string uid) {
    daw::InsertModel insert;
    insert.id = std::move(id);
    insert.name = uid;
    insert.format = daw::PluginFormat::Internal;
    insert.uid = std::move(uid);
    insert.path = "/Applications/VLT/internal";
    insert.pluginVersion = "1.0";
    insert.stateSchemaVersion = 1;
    return insert;
}

void writeTone(const fs::path& path) {
    constexpr audio::SampleRate rate = 48000;
    audio::AudioBuffer tone(2, 2048);
    for (audio::BufferSize frame = 0; frame < 2048; ++frame) {
        const float value = 0.25f * std::sin(
            2.0f * 3.14159265f * 220.0f * float(frame) / float(rate));
        tone.getChannel(0)[frame] = value;
        tone.getChannel(1)[frame] = value;
    }
    audio::AudioRecorder recorder;
    recorder.initialize(rate, 2);
    (void)recorder.writeWAVFile(path.string(), tone, rate);
}

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

template <typename Visitor>
void visitAssetRefs(daw::ProjectModel& project, Visitor&& visitor) {
    const auto insert = [&](daw::InsertModel& slot) {
        visitor(slot.stateAsset);
        visitor(slot.rightStateAsset);
        for (daw::PluginAssetBinding& binding : slot.assetBindings)
            visitor(binding.asset);
    };
    for (daw::InsertModel& slot : project.masterInserts) insert(slot);
    for (daw::TrackModel& track : project.tracks) {
        insert(track.instrument);
        for (daw::InsertModel& slot : track.samplerFx.inserts) insert(slot);
        for (daw::InsertModel& slot : track.inserts) insert(slot);
        for (daw::ClipModel& clip : track.clips) {
            visitor(clip.asset);
            for (daw::TakeModel& take : clip.takes) visitor(take.asset);
            for (daw::InsertModel& slot : clip.inserts) insert(slot);
        }
    }
}

void completeCapturedAssets(daw::cloud::CloudPublicationCapture& capture) {
    std::unordered_map<std::string, daw::AssetRef> completed;
    for (std::size_t index = 0; index < capture.sources.size(); ++index) {
        daw::AssetRef asset = capture.sources[index].asset;
        asset.sha256.assign(64, char('a' + int(index % 6)));
        capture.sources[index].asset = asset;
        completed.emplace(asset.assetId, std::move(asset));
    }
    visitAssetRefs(capture.document, [&](daw::AssetRef& asset) {
        if (asset.empty()) return;
        const auto found = completed.find(asset.assetId);
        if (found != completed.end()) asset = found->second;
    });
}

const daw::PluginAssetBinding* binding(const daw::InsertModel& insert,
                                       const std::string& key) {
    const auto found = std::find_if(
        insert.assetBindings.begin(), insert.assetBindings.end(),
        [&](const daw::PluginAssetBinding& item) { return item.key == key; });
    return found == insert.assetBindings.end() ? nullptr : &*found;
}

} // namespace

int main() {
    {
        daw::ProjectModel project;
        project.masterInserts.push_back(
            builtin("10000000-0000-4000-8000-000000000001", "daw.equalizer"));
        project.masterInserts.push_back(
            builtin("10000000-0000-4000-8000-000000000002", "daw.gravity"));
        daw::TrackModel track;
        track.id = "20000000-0000-4000-8000-000000000001";
        track.instrument =
            builtin("30000000-0000-4000-8000-000000000001", "daw.sampler");
        track.instrument.assetBindings.push_back(daw::PluginAssetBinding{
            "sample",
            asset("31000000-0000-4000-8000-000000000001",
                  daw::AssetKind::Audio, "sample.wav"),
            true,
        });
        project.tracks.push_back(std::move(track));

        const auto report = daw::cloud::inspectForPublishV1(project);
        check(report.canPublish(),
              "Sampler, Equalizer and Gravity are the complete v1 built-in set");

        project.masterInserts.front().stateSchemaVersion = 0;
        check(!daw::cloud::inspectForPublishV1(project).canPublish(),
              "a built-in without an exact state schema is blocked");
        project.masterInserts.front().stateSchemaVersion = 1;

        const std::string stableTrackId = project.tracks.front().id;
        project.tracks.front().id = "not-a-uuid";
        check(!daw::cloud::inspectForPublishV1(project).canPublish(),
              "publication rejects non-UUID shared entity ids");
        project.tracks.front().id = stableTrackId;

        project.tracks.front().instrument.assetBindings.clear();
        const auto samplerWithoutAsset =
            daw::cloud::inspectForPublishV1(project);
        check(!samplerWithoutAsset.canPublish() &&
                  samplerWithoutAsset.blockers.front().detail.find("sample") !=
                      std::string::npos,
              "a loaded Sampler without its required sample binding is blocked");
        project.tracks.front().instrument.assetBindings.push_back(
            daw::PluginAssetBinding{
                "sample",
                asset("31000000-0000-4000-8000-000000000001",
                      daw::AssetKind::Audio, "sample.wav"),
                true,
            });

        daw::InsertModel external;
        external.id = "40000000-0000-4000-8000-000000000001";
        external.name = "Filter Pro Q4";
        external.uid = "vendor.filter-pro-q4";
        external.format = daw::PluginFormat::Vst3;
        project.tracks.front().inserts.push_back(external);
        const auto blocked = daw::cloud::inspectForPublishV1(project);
        check(!blocked.canPublish() &&
                  blocked.blockers.front().kind ==
                      daw::cloud::PublishIssueKind::ThirdPartyPlugin,
              "a third-party slot blocks publication with a concrete issue");
    }

    {
        daw::ProjectModel project;
        project.loopEnabled = true;
        project.loopStartSeconds = 3.0;
        project.loopEndSeconds = 7.0;

        daw::TrackModel track;
        track.id = "50000000-0000-4000-8000-000000000001";
        track.soloed = true;
        track.armed = true;
        track.monitor = true;
        track.inputEnabled = true;
        track.inputChannel = 3;
        track.inputChannelCount = 2;
        track.height = 144.0;
        track.expanded = false;
        track.automationExpanded = true;

        daw::ClipModel clip;
        clip.id = "60000000-0000-4000-8000-000000000001";
        clip.kind = daw::ClipKind::Audio;
        clip.filePath = "/Users/example/secret/Kick.wav";
        clip.asset = asset("70000000-0000-4000-8000-000000000001",
                           daw::AssetKind::Audio,
                           "C:\\Users\\example\\secret\\Kick.wav");
        clip.expanded = true;

        daw::InsertModel effect =
            builtin("80000000-0000-4000-8000-000000000001", "daw.equalizer");
        effect.stateFile = "/Users/example/secret/eq.state";
        effect.stateAsset = asset("90000000-0000-4000-8000-000000000001",
                                  daw::AssetKind::PluginState,
                                  "/Users/example/secret/eq.state");
        effect.windowOpen = true;
        effect.windowX = 99;
        clip.inserts.push_back(std::move(effect));
        track.clips.push_back(std::move(clip));
        project.tracks.push_back(std::move(track));

        const auto cloud = daw::cloud::projectForCloudSnapshotV1(project);
        check(cloud.valid(),
              "a built-in-only project with completed assets can be projected");
        check(!daw::cloud::containsLocalPathOrUiState(cloud.document),
              "cloud projection contains no legacy path or local UI/session state");
        const auto& projectedTrack = cloud.document.tracks.front();
        const auto& projectedClip = projectedTrack.clips.front();
        check(!cloud.document.loopEnabled && !projectedTrack.soloed &&
                  !projectedTrack.armed && !projectedTrack.monitor &&
                  projectedClip.filePath.empty() && !projectedClip.expanded &&
                  projectedClip.asset.originalName == "Kick.wav" &&
                  projectedClip.inserts.front().path.empty() &&
                  projectedClip.inserts.front().stateFile.empty() &&
                  !projectedClip.inserts.front().windowOpen,
              "projection strips transport, capture, geometry and local paths");

        project.tracks.front().clips.front().asset = {};
        const auto missing = daw::cloud::projectForCloudSnapshotV1(project);
        check(!missing.valid(),
              "file-backed audio cannot enter a cloud snapshot before upload");
    }

    // The controller seam captures live built-in state without touching the
    // local project. Hashing is intentionally simulated here: the publisher,
    // not this seam, owns the real SHA-256 worker.
    {
        const fs::path root = fs::temp_directory_path() /
                              ("vlt-cloud-capture-test-" + daw::newUuid());
        const fs::path source = root / "private-source.wav";
        const fs::path stagingParent = root / "staging";
        std::error_code error;
        fs::create_directories(root, error);
        writeTone(source);

        daw::EngineController controller;
        check(controller.initialize(48000, 512, false).isOk(),
              "cloud capture fixture initializes without an audio device");
        const std::string audioTrack =
            controller.addTrack(daw::TrackKind::Audio, "Audio");
        check(!controller.importAudio(source.string(), audioTrack, 0.0).empty(),
              "cloud capture fixture imports local media");

        const std::string samplerTrack =
            controller.addTrack(daw::TrackKind::Instrument, "Sampler");
        const auto sampler = controller.pluginManager().find(
            daw::plugins::Format::Internal, "daw.sampler");
        check(sampler && controller.setTrackInstrumentPlugin(samplerTrack,
                                                              *sampler),
              "cloud capture fixture loads the built-in Sampler");
        const daw::TrackModel* samplerModel =
            controller.project().findTrack(samplerTrack);
        const std::string samplerSlot =
            samplerModel ? samplerModel->instrument.id : std::string{};
        check(!samplerSlot.empty() && controller.loadSamplerSample(
                                          samplerTrack, samplerSlot,
                                          source.string()),
              "cloud capture fixture loads the Sampler source");

        const auto equalizer = controller.pluginManager().find(
            daw::plugins::Format::Internal, "daw.equalizer");
        const std::string equalizerSlot =
            equalizer ? controller.addInsert(audioTrack, *equalizer)
                      : std::string{};
        check(!equalizerSlot.empty() && controller.setInsertChannelMode(
                                                audioTrack, equalizerSlot,
                                                daw::PluginChannelMode::DualMono),
              "cloud capture fixture has a dual-mono built-in state");

        controller.seekSeconds(1.25);
        const double positionBefore = controller.positionSeconds();
        const std::size_t undoBefore = controller.undoDepth();
        std::string documentBefore;
        std::string documentAfter;
        (void)daw::ProjectSerializer::serializeDocument(
            controller.project(), documentBefore);

        auto capture =
            controller.captureCloudPublicationV1(stagingParent.string());
        (void)daw::ProjectSerializer::serializeDocument(
            controller.project(), documentAfter);
        check(capture.readyForHashing(),
              "built-in-only live project produces an upload capture");
        check(documentAfter == documentBefore &&
                  controller.undoDepth() == undoBefore &&
                  std::abs(controller.positionSeconds() - positionBefore) < 1e-9,
              "capture leaves source document, undo and transport unchanged");
        check(!capture.stagingDirectory().empty() &&
                  fs::is_directory(capture.stagingDirectory()),
              "opaque plugin chunks live in an owned staging directory");

        const daw::TrackModel* capturedSampler =
            capture.document.findTrack(samplerTrack);
        const daw::TrackModel* capturedAudio =
            capture.document.findTrack(audioTrack);
        const daw::PluginAssetBinding* sampleBinding =
            capturedSampler
                ? binding(capturedSampler->instrument, "sample")
                : nullptr;
        const daw::ClipModel* capturedClip =
            capturedAudio && !capturedAudio->clips.empty()
                ? &capturedAudio->clips.front()
                : nullptr;
        check(sampleBinding && sampleBinding->required && capturedClip &&
                  !sampleBinding->asset.assetId.empty() &&
                  sampleBinding->asset.assetId == capturedClip->asset.assetId,
              "Sampler binding is explicit and identical media paths deduplicate");

        const daw::InsertModel* capturedEq = nullptr;
        if (capturedAudio) {
            const auto found = std::find_if(
                capturedAudio->inserts.begin(), capturedAudio->inserts.end(),
                [&](const daw::InsertModel& item) {
                    return item.id == equalizerSlot;
                });
            if (found != capturedAudio->inserts.end()) capturedEq = &*found;
        }
        check(capturedEq && !capturedEq->stateAsset.empty() &&
                  !capturedEq->rightStateAsset.empty() &&
                  capturedEq->stateAsset.assetId !=
                      capturedEq->rightStateAsset.assetId,
              "dual-mono capture stages both independent state chunks");

        const std::string samplerState =
            capturedSampler ? readFile(capturedSampler->instrument.stateFile)
                            : std::string{};
        const nlohmann::json samplerJson =
            nlohmann::json::parse(samplerState, nullptr, false);
        check(!samplerJson.is_discarded() &&
                  samplerJson.value("sample", std::string("not-empty")).empty() &&
                  samplerState.find(source.string()) == std::string::npos,
              "Sampler opaque state never persists its local sample path");

        const std::size_t audioSources = std::count_if(
            capture.sources.begin(), capture.sources.end(),
            [](const daw::cloud::LocalPublicationAssetSource& item) {
                return item.asset.kind == daw::AssetKind::Audio;
            });
        check(audioSources == 1 &&
                  std::all_of(
                      capture.sources.begin(), capture.sources.end(),
                      [](const daw::cloud::LocalPublicationAssetSource& item) {
                          return !item.asset.assetId.empty() &&
                                 item.asset.sha256.empty() &&
                                 item.asset.byteSize > 0 &&
                                 !item.asset.mimeType.empty();
                      }),
              "capture emits stable partial AssetRefs and defers hashing");

        completeCapturedAssets(capture);
        const auto projected =
            daw::cloud::projectForCloudSnapshotV1(capture.document);
        std::string cloudBytes;
        (void)daw::ProjectSerializer::serializeDocument(projected.document,
                                                         cloudBytes);
        check(projected.valid() &&
                  !daw::cloud::containsLocalPathOrUiState(projected.document) &&
                  cloudBytes.find(source.string()) == std::string::npos &&
                  cloudBytes.find(capture.stagingDirectory()) ==
                      std::string::npos,
              "completed capture projects to a path-free cloud document");

        const std::string ownedStaging = capture.stagingDirectory();
        check(capture.cleanup() && !fs::exists(ownedStaging) &&
                  capture.cleanup(),
              "capture staging cleanup is explicit and idempotent");

        std::string destructorStaging;
        {
            auto temporary =
                controller.captureCloudPublicationV1(stagingParent.string());
            destructorStaging = temporary.stagingDirectory();
            check(temporary.readyForHashing() &&
                      fs::is_directory(destructorStaging),
                  "a second capture owns a fresh staging generation");
        }
        check(!fs::exists(destructorStaging),
              "destroying a capture cleans its staging generation");

        fs::remove(source, error);
        auto missing =
            controller.captureCloudPublicationV1(stagingParent.string());
        check(!missing.readyForHashing() &&
                  missing.stagingDirectory().empty() &&
                  std::any_of(
                      missing.blockers.begin(), missing.blockers.end(),
                      [](const daw::cloud::PublicationCaptureIssue& issue) {
                          return issue.kind ==
                              daw::cloud::PublicationCaptureIssueKind::MissingLocalSource;
                      }),
              "missing local media is reported before staging is created");

        writeTone(source);
        const fs::path badParent = root / "not-a-directory";
        {
            std::ofstream file(badParent);
            file << "occupied";
        }
        auto stagingFailure =
            controller.captureCloudPublicationV1(badParent.string());
        check(!stagingFailure.readyForHashing() &&
                  stagingFailure.stagingDirectory().empty() &&
                  std::any_of(
                      stagingFailure.blockers.begin(),
                      stagingFailure.blockers.end(),
                      [](const daw::cloud::PublicationCaptureIssue& issue) {
                          return issue.kind ==
                              daw::cloud::PublicationCaptureIssueKind::StagingIo;
                      }),
              "staging failures leave no partial generation behind");

        daw::EngineController incompatible;
        (void)incompatible.initialize(48000, 512, false);
        daw::ProjectModel incompatibleProject;
        daw::TrackModel incompatibleTrack;
        incompatibleTrack.id = daw::newUuid();
        incompatibleTrack.kind = daw::TrackKind::Audio;
        daw::InsertModel external;
        external.id = daw::newUuid();
        external.name = "Filter Pro Q4";
        external.uid = "vendor.filter-pro-q4";
        external.format = daw::PluginFormat::Vst3;
        incompatibleTrack.inserts.push_back(external);
        incompatibleProject.tracks.push_back(std::move(incompatibleTrack));
        (void)incompatible.materializeCollaborationProject(
            std::move(incompatibleProject), true);
        auto blocked =
            incompatible.captureCloudPublicationV1(stagingParent.string());
        check(!blocked.readyForHashing() &&
                  blocked.stagingDirectory().empty() &&
                  !blocked.blockers.empty() &&
                  blocked.blockers.front().kind ==
                      daw::cloud::PublicationCaptureIssueKind::ThirdPartyPlugin &&
                  blocked.blockers.front().displayName == "Filter Pro Q4" &&
                  blocked.blockers.front().pluginUid == "vendor.filter-pro-q4",
              "third-party compatibility blocker is concrete and precedes staging");

        controller.shutdown();
        incompatible.shutdown();
        fs::remove_all(root, error);
    }

    if (failures) {
        std::printf("%d cloud project test(s) failed\n", failures);
        return 1;
    }
    std::puts("All cloud project tests passed");
    return 0;
}
