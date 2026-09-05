#include "collaboration/CommandJson.hpp"
#include "collaboration/ProjectReducer.hpp"
#include "collaboration/RecordingCommitPlanner.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <variant>

using namespace daw;
using namespace daw::collab;
using namespace daw::recovery;

namespace {

int failures = 0;

bool check(bool condition, const char* label) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
    return condition;
}

std::string id(std::string_view domain, std::string_view label) {
    return deterministicMigrationId(domain, label);
}

AssetRef audioAsset(std::string_view label, std::uint32_t channels = 2,
                    std::uint64_t frames = 96000) {
    AssetRef asset;
    asset.assetId = id("planner-asset", label);
    asset.sha256 = std::string(64, label.empty() ? 'a' : 'b');
    asset.kind = AssetKind::Audio;
    asset.byteSize = frames * channels * 3;
    asset.originalName = "recording.wav";
    asset.mimeType = "audio/wav";
    asset.codec = "pcm_s24le";
    asset.sampleRate = 48000.0;
    asset.channels = channels;
    asset.frames = frames;
    return asset;
}

struct Fixture {
    SharedProjectDocument snapshot;
    CloudRecordingRecoveryRun run;
    RecordingCommitPlanInput input;
};

Fixture flatFixture(std::string_view suffix = "flat") {
    Fixture fixture;
    fixture.snapshot.confirmedSequence = 17;

    TrackModel track;
    track.id = id("planner-track", suffix);
    track.kind = TrackKind::Audio;
    track.name = "Audio";
    track.color = 0x123456;
    fixture.snapshot.project.tracks.push_back(track);

    fixture.run.runId = id("planner-run", suffix);
    fixture.run.opId = id("planner-operation", suffix);
    fixture.run.transactionId = id("planner-transaction", suffix);
    fixture.run.createdAtUnixMs = 1;

    CloudRecordingCapture capture;
    capture.captureId = id("planner-capture", suffix);
    capture.trackId = track.id;
    capture.leaseId = id("planner-lease", suffix);
    capture.uploadId = id("planner-upload", suffix);
    capture.assetId = id("planner-asset", suffix);
    capture.localWavPath = "/deliberately/not/read/recording.wav";
    capture.startSeconds = 4.0;
    capture.durationSeconds = 2.0;
    capture.sampleRate = 48000.0;
    capture.channels = 2;
    capture.frames = 96000;
    capture.passes = {{4.0, 6.0, 0.0}};
    capture.semantics.mode = CloudRecordingMode::Overwrite;
    fixture.run.captures.push_back(capture);

    fixture.input.meta.projectId = id("planner-project", "project");
    fixture.input.meta.operationId = fixture.run.opId;
    fixture.input.meta.transactionId = fixture.run.transactionId;
    fixture.input.meta.baseServerSequence = fixture.snapshot.confirmedSequence;
    // actor/client provenance is intentionally absent from the canonical wire.
    fixture.input.meta.clientSequence = 0;
    fixture.input.leases.push_back({capture.trackId, capture.leaseId});

    RecordingCommitCaptureInput captureInput;
    captureInput.captureId = capture.captureId;
    captureInput.uploadedAsset = audioAsset(suffix);
    captureInput.clips.push_back(
        {id("planner-clip", suffix), std::string()});
    fixture.input.captures.push_back(std::move(captureInput));
    return fixture;
}

Fixture layeredFixture() {
    Fixture fixture = flatFixture("layers");
    CloudRecordingCapture& capture = fixture.run.captures.front();
    capture.startSeconds = 2.0;
    capture.durationSeconds = 5.0;
    capture.frames = 240000;
    capture.semantics.mode = CloudRecordingMode::Layers;
    capture.semantics.loopEnabled = true;
    capture.semantics.loopStartSeconds = 0.0;
    capture.semantics.loopEndSeconds = 4.0;
    capture.semantics.compCrossfadeMs = 7.25;
    capture.passes = {{2.0, 4.0, 0.0}, {0.0, 3.0, 2.0}};

    RecordingCommitCaptureInput& input = fixture.input.captures.front();
    input.uploadedAsset = audioAsset("layers", 2, 240000);
    input.clips.clear();
    input.containerClipId = id("planner-container", "layers");
    const std::string takeOne = id("planner-take", "layers-1");
    const std::string takeTwo = id("planner-take", "layers-2");
    input.takes = {{takeOne, {}}, {takeTwo, takeOne}};
    const std::string segmentOne = id("planner-comp", "layers-1");
    const std::string segmentTwo = id("planner-comp", "layers-2");
    input.compSegments = {{segmentOne, {}}, {segmentTwo, segmentOne}};
    return fixture;
}

RecordingCommitPreflightInput preflightInput(const Fixture& fixture) {
    RecordingCommitPreflightInput input;
    input.baseServerSequence = fixture.input.meta.baseServerSequence;
    input.leases = fixture.input.leases;
    input.operationAbsenceProof = fixture.input.operationAbsenceProof;
    return input;
}

ProjectCommand assetCommand(const std::string& operationId,
                            const std::string& projectId,
                            const std::string& trackId,
                            const std::string& clipId, AssetRef asset) {
    ProjectCommand command;
    command.meta.projectId = projectId;
    command.meta.operationId = operationId;
    command.body = SetClipAsset{trackId, clipId, std::move(asset)};
    return command;
}

} // namespace

int main() {
    Fixture flat = flatFixture();
    check(RecordingCommitPlanner::preflight(flat.snapshot, flat.run,
                                            preflightInput(flat))
              .ready(),
          "flat capture passes the pure pre-upload gate");
    RecordingCommitPlanResult planned = RecordingCommitPlanner::plan(
        flat.snapshot, flat.run, flat.input);
    const auto* recording =
        planned.command
            ? std::get_if<RecordingCommit>(&planned.command->body)
            : nullptr;
    check(planned && recording && recording->batch &&
              commandKind(*planned.command) == "recording.commit" &&
              recording->leases.size() == 1 &&
              recording->batch->commands.size() == 2,
          "flat capture plans one canonical recording.commit");

    std::string wireError;
    const std::string flatWire =
        planned ? serializeProjectCommand(*planned.command) : std::string();
    const RecordingCommitPlanResult plannedAgain = RecordingCommitPlanner::plan(
        flat.snapshot, flat.run, flat.input);
    check(plannedAgain &&
              serializeProjectCommand(*plannedAgain.command) == flatWire &&
              deserializeProjectCommand(flatWire, &wireError).has_value(),
          "planner is deterministic and emits round-trippable canonical wire");

    Fixture leaseFree = flatFixture("lease-free-v3");
    leaseFree.run.captures.front().leaseId.clear();
    leaseFree.input.leases.clear();
    ClipModel simultaneous;
    simultaneous.id = id("planner-existing-clip", "lease-free-v3");
    simultaneous.kind = ClipKind::Audio;
    simultaneous.startSeconds = 4.0;
    simultaneous.durationSeconds = 2.0;
    leaseFree.snapshot.project.tracks.front().clips.push_back(simultaneous);
    const RecordingCommitPlanResult leaseFreePlan =
        RecordingCommitPlanner::plan(leaseFree.snapshot, leaseFree.run,
                                     leaseFree.input);
    const auto* leaseFreeCommit =
        leaseFreePlan.command
            ? std::get_if<RecordingCommit>(&leaseFreePlan.command->body)
            : nullptr;
    check(leaseFreePlan && leaseFreeCommit && leaseFreeCommit->leases.empty() &&
              leaseFreeCommit->batch &&
              leaseFreeCommit->batch->commands.size() == 2 &&
              deserializeProjectCommand(
                  serializeProjectCommand(*leaseFreePlan.command), &wireError)
                  .has_value(),
          "v3 overlapping new-clip recording plans without a track lease");
    if (leaseFreePlan.command) {
        auto v2Wire = projectCommandToJson(*leaseFreePlan.command);
        v2Wire["schemaVersion"] = kProjectCommandSchemaVersionV2;
        check(!projectCommandFromJson(v2Wire, &wireError).has_value(),
              "v2 keeps the non-empty recording lease contract");
    }

    SharedProjectDocument flatApplied = flat.snapshot;
    const ApplyResult flatResult =
        planned ? ProjectReducer::apply(flatApplied, *planned.command)
                : ApplyResult{};
    const TrackModel* flatTrack =
        flatApplied.project.findTrack(flat.run.captures.front().trackId);
    const ClipModel* flatClip =
        flatTrack && flatTrack->clips.size() == 1 ? &flatTrack->clips.front()
                                                  : nullptr;
    check(flatResult.changed() && flatClip &&
              flatClip->kind == ClipKind::Audio &&
              flatClip->asset == flat.input.captures.front().uploadedAsset &&
              flatClip->channels == 2 && flatClip->filePath.empty(),
          "flat landing carries AssetRef channels without local path access");

    Fixture rebindWithoutProof = flatFixture("rebind-no-proof");
    const std::string freshLeaseWithoutProof =
        id("planner-fresh-lease", "rebind-no-proof");
    rebindWithoutProof.input.leases.front().leaseId = freshLeaseWithoutProof;
    check(RecordingCommitPlanner::preflight(
              rebindWithoutProof.snapshot, rebindWithoutProof.run,
              preflightInput(rebindWithoutProof))
                  .code == RecordingCommitPlanCode::SnapshotConflict &&
              RecordingCommitPlanner::plan(
                  rebindWithoutProof.snapshot, rebindWithoutProof.run,
                  rebindWithoutProof.input)
                      .code == RecordingCommitPlanCode::SnapshotConflict,
          "fresh lease is rejected without authoritative operation absence "
          "proof");

    Fixture wrongRebindProof = flatFixture("rebind-wrong-proof");
    wrongRebindProof.input.leases.front().leaseId =
        id("planner-fresh-lease", "rebind-wrong-proof");
    wrongRebindProof.input.operationAbsenceProof = {
        id("planner-operation", "another-operation"),
        wrongRebindProof.snapshot.confirmedSequence};
    Fixture staleRebindProof = flatFixture("rebind-stale-proof");
    staleRebindProof.input.leases.front().leaseId =
        id("planner-fresh-lease", "rebind-stale-proof");
    staleRebindProof.input.operationAbsenceProof = {
        staleRebindProof.run.opId,
        staleRebindProof.snapshot.confirmedSequence - 1};
    check(RecordingCommitPlanner::plan(
              wrongRebindProof.snapshot, wrongRebindProof.run,
              wrongRebindProof.input)
                  .code == RecordingCommitPlanCode::SnapshotConflict &&
              RecordingCommitPlanner::plan(
                  staleRebindProof.snapshot, staleRebindProof.run,
                  staleRebindProof.input)
                      .code == RecordingCommitPlanCode::SnapshotConflict,
          "wrong-operation and stale-head absence proofs are rejected");

    Fixture provenRebind = flatFixture("rebind-proven");
    const std::string provenFreshLease =
        id("planner-fresh-lease", "rebind-proven");
    const std::string provenFrozenLease =
        provenRebind.run.captures.front().leaseId;
    provenRebind.input.leases.front().leaseId = provenFreshLease;
    provenRebind.input.operationAbsenceProof = {
        provenRebind.run.opId, provenRebind.snapshot.confirmedSequence};
    const RecordingCommitPreflightResult provenRebindPreflight =
        RecordingCommitPlanner::preflight(
            provenRebind.snapshot, provenRebind.run,
            preflightInput(provenRebind));
    const RecordingCommitPlanResult provenRebindPlan =
        RecordingCommitPlanner::plan(provenRebind.snapshot,
                                     provenRebind.run,
                                     provenRebind.input);
    const auto* provenRebindRecording =
        provenRebindPlan.command
            ? std::get_if<RecordingCommit>(&provenRebindPlan.command->body)
            : nullptr;
    SharedProjectDocument provenRebindApplied = provenRebind.snapshot;
    const ApplyResult provenRebindResult =
        provenRebindPlan
            ? ProjectReducer::apply(provenRebindApplied,
                                    *provenRebindPlan.command)
            : ApplyResult{};
    check(provenRebindPreflight.ready() && provenRebindPlan &&
              provenRebindRecording &&
              provenRebindRecording->leases.size() == 1 &&
              provenRebindRecording->leases.front().leaseId ==
                  provenFreshLease &&
              provenRebindRecording->leases.front().leaseId !=
                  provenFrozenLease &&
              provenRebindResult.changed(),
          "exact authoritative absence proof rebinds and emits only the fresh "
          "lease");

    Fixture layers = layeredFixture();
    RecordingCommitPlanResult layered = RecordingCommitPlanner::plan(
        layers.snapshot, layers.run, layers.input);
    const auto* layeredRecording =
        layered.command
            ? std::get_if<RecordingCommit>(&layered.command->body)
            : nullptr;
    SharedProjectDocument layersApplied = layers.snapshot;
    const ApplyResult layerResult =
        layered ? ProjectReducer::apply(layersApplied, *layered.command)
                : ApplyResult{};
    const TrackModel* layerTrack =
        layersApplied.project.findTrack(layers.run.captures.front().trackId);
    const ClipModel* layerClip =
        layerTrack && layerTrack->clips.size() == 1
            ? &layerTrack->clips.front()
            : nullptr;
    const bool crossfadeImmediatelyAfterAdd =
        layeredRecording && layeredRecording->batch &&
        layeredRecording->batch->commands.size() == 6 &&
        std::holds_alternative<AddClip>(
            layeredRecording->batch->commands[0].body) &&
        std::holds_alternative<SetClipProperty>(
            layeredRecording->batch->commands[1].body) &&
        std::get<SetClipProperty>(
            layeredRecording->batch->commands[1].body).property ==
            ClipProperty::CompCrossfadeMs;
    check(layerResult.changed() && crossfadeImmediatelyAfterAdd &&
              layerClip && layerClip->channels == 0 &&
              layerClip->asset.empty() && layerClip->takes.size() == 2 &&
              std::fabs(layerClip->compCrossfadeMs - 7.25) < 1e-9 &&
              layerClip->takes[0].channels == 2 &&
              layerClip->takes[1].channels == 2 &&
              layerClip->comp.size() == 2 &&
              layerClip->comp[0].takeId == layerClip->takes[1].id &&
              layerClip->comp[0].startSeconds == 0.0 &&
              layerClip->comp[0].endSeconds == 3.0 &&
              layerClip->comp[1].takeId == layerClip->takes[0].id &&
              layerClip->comp[1].startSeconds == 3.0 &&
              layerClip->comp[1].endSeconds == 4.0,
          "layer landing freezes crossfade before takes and forms an unambiguous comp container");

    Fixture trimmed = layeredFixture();
    CloudRecordingCapture& trimmedCapture = trimmed.run.captures.front();
    trimmedCapture.durationSeconds = 8.0;
    trimmedCapture.frames = 384000;
    trimmedCapture.semantics.trimTakesToRegion = true;
    trimmedCapture.passes = {
        {2.0, 4.0, 0.0}, {0.0, 4.0, 2.0}, {0.0, 2.0, 6.0}};
    RecordingCommitCaptureInput& trimmedInput = trimmed.input.captures.front();
    trimmedInput.uploadedAsset = audioAsset("layers", 2, 384000);
    const std::string trimmedTakeOne = id("planner-take", "trimmed-1");
    const std::string trimmedTakeTwo = id("planner-take", "trimmed-2");
    const std::string trimmedTakeThree = id("planner-take", "trimmed-3");
    trimmedInput.takes = {{trimmedTakeOne, {}},
                          {trimmedTakeTwo, trimmedTakeOne},
                          {trimmedTakeThree, trimmedTakeTwo}};
    const std::string trimmedCompOne = id("planner-comp", "trimmed-1");
    const std::string trimmedCompTwo = id("planner-comp", "trimmed-2");
    trimmedInput.compSegments = {{trimmedCompOne, {}},
                                 {trimmedCompTwo, trimmedCompOne}};
    const RecordingCommitPlanResult trimmedPlan = RecordingCommitPlanner::plan(
        trimmed.snapshot, trimmed.run, trimmed.input);
    SharedProjectDocument trimmedApplied = trimmed.snapshot;
    const ApplyResult trimmedResult =
        trimmedPlan
            ? ProjectReducer::apply(trimmedApplied, *trimmedPlan.command)
            : ApplyResult{};
    const ClipModel* trimmedClip =
        trimmedApplied.project.tracks.front().clips.empty()
            ? nullptr
            : &trimmedApplied.project.tracks.front().clips.front();
    check(trimmedResult.changed() && trimmedClip &&
              trimmedClip->takes.size() == 3 && trimmedClip->comp.size() == 2 &&
              trimmedClip->comp[0].takeId == trimmedTakeThree &&
              trimmedClip->comp[0].startSeconds == 0.0 &&
              trimmedClip->comp[0].endSeconds == 2.0 &&
              trimmedClip->comp[1].takeId == trimmedTakeTwo &&
              trimmedClip->comp[1].startSeconds == 2.0 &&
              trimmedClip->comp[1].endSeconds == 4.0,
          "three overlapping trimmed takes produce the newest visible comp pieces");

    Fixture autoExpand = layers;
    autoExpand.run.captures.front().semantics.autoExpandAfterRecord = true;
    const RecordingCommitPlanResult autoExpanded = RecordingCommitPlanner::plan(
        autoExpand.snapshot, autoExpand.run, autoExpand.input);
    check(autoExpanded && layered &&
              serializeProjectCommand(*autoExpanded.command) ==
                  serializeProjectCommand(*layered.command),
          "local auto-expand preference neither blocks nor changes shared commit");

    Fixture multi = flatFixture("multi-a");
    TrackModel secondTrack;
    secondTrack.id = id("planner-track", "multi-b");
    secondTrack.kind = TrackKind::Audio;
    secondTrack.color = 0x654321;
    multi.snapshot.project.tracks.push_back(secondTrack);
    CloudRecordingCapture secondCapture = multi.run.captures.front();
    secondCapture.captureId = id("planner-capture", "multi-b");
    secondCapture.trackId = secondTrack.id;
    secondCapture.leaseId = id("planner-lease", "multi-b");
    secondCapture.uploadId = id("planner-upload", "multi-b");
    secondCapture.assetId = id("planner-asset", "multi-b");
    secondCapture.startSeconds = 8.0;
    secondCapture.passes = {{8.0, 10.0, 0.0}};
    multi.run.captures.push_back(secondCapture);
    multi.input.leases.push_back(
        {secondCapture.trackId, secondCapture.leaseId});
    RecordingCommitCaptureInput secondInput;
    secondInput.captureId = secondCapture.captureId;
    secondInput.uploadedAsset = audioAsset("multi-b");
    secondInput.clips = {{id("planner-clip", "multi-b"), {}}};
    multi.input.captures.push_back(secondInput);
    const RecordingCommitPlanResult multiPlanned = RecordingCommitPlanner::plan(
        multi.snapshot, multi.run, multi.input);
    SharedProjectDocument multiApplied = multi.snapshot;
    const ApplyResult multiResult =
        multiPlanned
            ? ProjectReducer::apply(multiApplied, *multiPlanned.command)
            : ApplyResult{};
    check(multiResult.changed() &&
              multiApplied.project.findTrack(
                  multi.run.captures[0].trackId)->clips.size() == 1 &&
              multiApplied.project.findTrack(
                  multi.run.captures[1].trackId)->clips.size() == 1,
          "multiple capture assets land atomically on distinct leased tracks");

    Fixture tooMany = flatFixture("bound-0");
    for (std::size_t index = 1; index < 9; ++index) {
        const std::string label = "bound-" + std::to_string(index);
        TrackModel boundedTrack;
        boundedTrack.id = id("planner-track", label);
        boundedTrack.kind = TrackKind::Audio;
        tooMany.snapshot.project.tracks.push_back(boundedTrack);
        CloudRecordingCapture boundedCapture = tooMany.run.captures.front();
        boundedCapture.captureId = id("planner-capture", label);
        boundedCapture.trackId = boundedTrack.id;
        boundedCapture.leaseId = id("planner-lease", label);
        boundedCapture.uploadId = id("planner-upload", label);
        boundedCapture.assetId = id("planner-asset", label);
        boundedCapture.startSeconds = 12.0 + double(index) * 3.0;
        boundedCapture.passes = {
            {boundedCapture.startSeconds,
             boundedCapture.startSeconds + 2.0, 0.0}};
        tooMany.run.captures.push_back(boundedCapture);
        tooMany.input.leases.push_back(
            {boundedCapture.trackId, boundedCapture.leaseId});
        RecordingCommitCaptureInput boundedInput;
        boundedInput.captureId = boundedCapture.captureId;
        boundedInput.uploadedAsset = audioAsset(label);
        boundedInput.clips = {{id("planner-clip", label), {}}};
        tooMany.input.captures.push_back(std::move(boundedInput));
    }
    check(RecordingCommitPlanner::plan(tooMany.snapshot, tooMany.run,
                                       tooMany.input)
                  .code == RecordingCommitPlanCode::InvalidInput,
          "planner enforces the eight-track recording runtime bound");

    Fixture manyPasses = flatFixture("many-passes");
    manyPasses.run.captures.front().semantics.mode =
        CloudRecordingMode::Layers;
    manyPasses.run.captures.front().passes.resize(
        kMaxCloudRecordingPassesPerCapture);
    const RecordingCommitPreflightResult manyPassesPreflight =
        RecordingCommitPlanner::preflight(
            manyPasses.snapshot, manyPasses.run,
            preflightInput(manyPasses));
    const RecordingCommitPlanResult manyPassesPlan =
        RecordingCommitPlanner::plan(manyPasses.snapshot, manyPasses.run,
                                     manyPasses.input);
    check(manyPassesPreflight.code ==
                  RecordingCommitPlanCode::UnsupportedSemantics &&
              manyPassesPlan.code ==
                  RecordingCommitPlanCode::UnsupportedSemantics &&
              manyPassesPreflight.message.find("bounded pass") !=
                  std::string::npos &&
              manyPassesPlan.message == manyPassesPreflight.message,
          "recovery-sized loop-pass input fails at the O(1) pass bound before comp planning");

    Fixture batchOverflow = flatFixture("batch-budget-a");
    CloudRecordingCapture& firstBudgetCapture =
        batchOverflow.run.captures.front();
    firstBudgetCapture.semantics.mode = CloudRecordingMode::Layers;
    firstBudgetCapture.passes.resize(200);
    TrackModel secondBudgetTrack;
    secondBudgetTrack.id = id("planner-track", "batch-budget-b");
    secondBudgetTrack.kind = TrackKind::Audio;
    batchOverflow.snapshot.project.tracks.push_back(secondBudgetTrack);
    CloudRecordingCapture secondBudgetCapture = firstBudgetCapture;
    secondBudgetCapture.captureId = id("planner-capture", "batch-budget-b");
    secondBudgetCapture.trackId = secondBudgetTrack.id;
    secondBudgetCapture.leaseId = id("planner-lease", "batch-budget-b");
    secondBudgetCapture.uploadId = id("planner-upload", "batch-budget-b");
    secondBudgetCapture.assetId = id("planner-asset", "batch-budget-b");
    batchOverflow.run.captures.push_back(secondBudgetCapture);
    batchOverflow.input.leases.push_back(
        {secondBudgetCapture.trackId, secondBudgetCapture.leaseId});
    const RecordingCommitPreflightResult overflowPreflight =
        RecordingCommitPlanner::preflight(
            batchOverflow.snapshot, batchOverflow.run,
            preflightInput(batchOverflow));
    const RecordingCommitPlanResult overflowPlan =
        RecordingCommitPlanner::plan(batchOverflow.snapshot,
                                     batchOverflow.run,
                                     batchOverflow.input);
    check(overflowPreflight.code ==
                  RecordingCommitPlanCode::UnsupportedSemantics &&
              overflowPlan.code ==
                  RecordingCommitPlanCode::UnsupportedSemantics &&
              overflowPreflight.message.find("batch limit") !=
                  std::string::npos &&
              overflowPlan.message == overflowPreflight.message,
          "aggregate worst-case command budget fails before pass geometry or asset inputs");

    Fixture overlap = flatFixture("overlap");
    ClipModel occupied;
    occupied.id = id("planner-existing-clip", "overlap");
    occupied.kind = ClipKind::Audio;
    occupied.startSeconds = 5.0;
    occupied.durationSeconds = 1.0;
    overlap.snapshot.project.tracks.front().clips.push_back(occupied);
    const RecordingCommitPreflightResult overlapPreflight =
        RecordingCommitPlanner::preflight(overlap.snapshot, overlap.run,
                                          preflightInput(overlap));
    check(overlapPreflight.code ==
                  RecordingCommitPlanCode::SnapshotConflict &&
              RecordingCommitPlanner::plan(overlap.snapshot, overlap.run,
                                            overlap.input)
                      .code == RecordingCommitPlanCode::SnapshotConflict,
          "existing punch target fails in preflight before upload");

    Fixture unknownLength = flatFixture("unknown-length");
    occupied.id = id("planner-existing-clip", "unknown-length");
    occupied.startSeconds = 0.0;
    occupied.durationSeconds = 0.0;
    occupied.asset = {};
    unknownLength.snapshot.project.tracks.front().clips.push_back(occupied);
    check(RecordingCommitPlanner::plan(unknownLength.snapshot,
                                       unknownLength.run,
                                       unknownLength.input)
                  .code == RecordingCommitPlanCode::SnapshotConflict,
          "unknown existing clip extent fails closed");

    Fixture overlappingOverwrite = flatFixture("overwrite-overlap");
    CloudRecordingCapture& overwriteCapture =
        overlappingOverwrite.run.captures.front();
    overwriteCapture.durationSeconds = 4.0;
    overwriteCapture.frames = 192000;
    overwriteCapture.semantics.loopEnabled = true;
    overwriteCapture.semantics.loopStartSeconds = 4.0;
    overwriteCapture.semantics.loopEndSeconds = 6.0;
    overwriteCapture.passes = {{4.0, 6.0, 0.0}, {4.0, 6.0, 2.0}};
    auto& overwriteInput = overlappingOverwrite.input.captures.front();
    overwriteInput.uploadedAsset =
        audioAsset("overwrite-overlap", 2, 192000);
    const std::string firstOverwriteClip =
        id("planner-clip", "overwrite-overlap-1");
    overwriteInput.clips = {
        {firstOverwriteClip, {}},
        {id("planner-clip", "overwrite-overlap-2"), firstOverwriteClip}};
    check(RecordingCommitPlanner::plan(overlappingOverwrite.snapshot,
                                       overlappingOverwrite.run,
                                       overlappingOverwrite.input)
                  .code == RecordingCommitPlanCode::UnsupportedSemantics,
          "overwrite that needs deletion of an earlier pass fails closed");

    Fixture unsafe = flatFixture("unsafe");
    unsafe.run.recoveryOnly = true;
    check(RecordingCommitPlanner::plan(unsafe.snapshot, unsafe.run,
                                       unsafe.input)
                  .code == RecordingCommitPlanCode::UnsafeRecovery,
          "recoveryOnly capture cannot become a shared commit");
    unsafe = flatFixture("lost-lease");
    unsafe.run.lostLease = true;
    check(RecordingCommitPlanner::plan(unsafe.snapshot, unsafe.run,
                                       unsafe.input)
                  .code == RecordingCommitPlanCode::UnsafeRecovery,
          "lost lease capture cannot become a shared commit");

    Fixture incomplete = flatFixture("incomplete");
    incomplete.run.captures.front().semantics.complete = false;
    check(RecordingCommitPlanner::plan(incomplete.snapshot, incomplete.run,
                                       incomplete.input)
                  .code == RecordingCommitPlanCode::UnsupportedSemantics,
          "incomplete migrated recording semantics fail closed");

    Fixture badStatus = flatFixture("bad-status");
    badStatus.run.captures.front().status =
        CloudRecordingCaptureStatus::WriteFailed;
    check(RecordingCommitPlanner::plan(badStatus.snapshot, badStatus.run,
                                       badStatus.input)
                  .code == RecordingCommitPlanCode::InvalidInput,
          "non-ready capture status fails closed");

    Fixture badPass = flatFixture("bad-pass");
    badPass.run.captures.front().passes.front().endSeconds = 5.5;
    check(RecordingCommitPlanner::plan(badPass.snapshot, badPass.run,
                                       badPass.input)
                  .code == RecordingCommitPlanCode::InvalidInput,
          "pass geometry inconsistent with frozen capture fails closed");

    Fixture badAsset = flatFixture("bad-asset");
    badAsset.input.captures.front().uploadedAsset.sha256[0] = 'A';
    check(RecordingCommitPlanner::plan(badAsset.snapshot, badAsset.run,
                                       badAsset.input)
                  .code == RecordingCommitPlanCode::InvalidInput,
          "uploaded asset must be a complete immutable audio AssetRef");

    bool privateNamesRejected = true;
    for (const std::string& forged : {
             std::string("/Users/alice/take.wav"),
             std::string("folder\\take.wav"),
             std::string("https://example.test/take.wav"),
             std::string("take\n.wav")}) {
        Fixture privateName = flatFixture("private-name");
        privateName.input.captures.front().uploadedAsset.originalName = forged;
        privateNamesRejected =
            privateNamesRejected &&
            RecordingCommitPlanner::plan(privateName.snapshot,
                                         privateName.run,
                                         privateName.input)
                    .code == RecordingCommitPlanCode::InvalidInput;
    }
    check(privateNamesRejected,
          "asset display name cannot leak a path, URL or control text");

    Fixture maximumDisplayName = flatFixture("display-name-255");
    maximumDisplayName.input.captures.front().uploadedAsset.originalName =
        std::string(251, 'a') + ".wav";
    Fixture oversizedDisplayName = flatFixture("display-name-256");
    oversizedDisplayName.input.captures.front().uploadedAsset.originalName =
        std::string(252, 'a') + ".wav";
    check(RecordingCommitPlanner::plan(
              maximumDisplayName.snapshot, maximumDisplayName.run,
              maximumDisplayName.input) &&
              RecordingCommitPlanner::plan(
                  oversizedDisplayName.snapshot, oversizedDisplayName.run,
                  oversizedDisplayName.input)
                      .code == RecordingCommitPlanCode::InvalidInput,
          "asset display name is bounded to 255 UTF-8 bytes");

    Fixture badId = flatFixture("bad-id");
    badId.input.captures.front().clips.front().id =
        "00000000-0000-0000-0000-000000000000";
    check(RecordingCommitPlanner::plan(badId.snapshot, badId.run,
                                       badId.input)
                  .code == RecordingCommitPlanCode::InvalidInput,
          "non-canonical caller entity UUID fails closed");

    Fixture badAnchor = flatFixture("bad-anchor");
    badAnchor.input.captures.front().clips.front().afterId =
        id("planner-clip", "missing-anchor");
    check(RecordingCommitPlanner::plan(badAnchor.snapshot, badAnchor.run,
                                       badAnchor.input)
                  .code == RecordingCommitPlanCode::InvalidInput,
          "caller anchor must exist in the current track");

    Fixture stale = flatFixture("stale");
    stale.input.meta.baseServerSequence -= 1;
    check(RecordingCommitPlanner::plan(stale.snapshot, stale.run, stale.input)
                  .code == RecordingCommitPlanCode::SnapshotConflict,
          "stale recording base sequence fails before command creation");

    // `clip.channels` is derived under the asset field and follows its inverse.
    SharedProjectDocument channelState;
    TrackModel channelTrack;
    channelTrack.id = id("planner-channel-track", "track");
    channelTrack.kind = TrackKind::Audio;
    ClipModel channelClip;
    channelClip.id = id("planner-channel-clip", "clip");
    channelClip.kind = ClipKind::Audio;
    channelClip.durationSeconds = 1.0;
    const AssetRef mono = audioAsset("channel-mono", 1, 48000);
    const AssetRef stereo = audioAsset("channel-stereo", 2, 48000);
    channelClip.asset = mono;
    channelClip.channels = 1;
    channelTrack.clips.push_back(channelClip);
    channelState.project.tracks.push_back(channelTrack);
    ProjectCommand replaceAsset = assetCommand(
        id("planner-channel-operation", "replace"),
        id("planner-channel-project", "project"), channelTrack.id,
        channelClip.id, stereo);
    ApplyResult replaced = ProjectReducer::apply(channelState, replaceAsset);
    ProjectCommand restoreAsset = replaced.inverse ? *replaced.inverse
                                                   : ProjectCommand{};
    restoreAsset.meta.projectId = replaceAsset.meta.projectId;
    restoreAsset.meta.operationId =
        id("planner-channel-operation", "restore");
    const ApplyResult restored =
        replaced.inverse ? ProjectReducer::apply(channelState, restoreAsset)
                         : ApplyResult{};
    const ClipModel& restoredClip =
        channelState.project.tracks.front().clips.front();
    check(replaced.changed() && restored.changed() &&
              restoredClip.asset == mono && restoredClip.channels == 1,
          "clip.setAsset channels are deterministic and inverse-consistent");

    if (failures)
        std::printf("%d recording commit planner test(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
