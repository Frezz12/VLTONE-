#pragma once

#include "collaboration/CollaborationState.hpp"
#include "recovery/CloudRecordingRecovery.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace daw::collab {

inline constexpr std::size_t kMaxRecordingCommitCaptures = 8;
/// Overwrite needs at most three children per pass (clip, offset, asset).
/// Layers needs a container, its frozen crossfade, N takes and at most
/// (2N - 1) comp pieces: 3N + 1.
/// Deriving this limit from the atomic batch cap keeps even a single hostile
/// capture bounded before the planner walks or composes its pass geometry.
inline constexpr std::size_t kMaxRecordingCommitPassesPerCapture =
    (kMaxProjectCommandBatchSize - 1) / 3;

/// Caller-owned durable identity plus its insertion anchor. The planner never
/// generates UUIDs and never guesses a position in an existing collection.
struct RecordingCommitAnchoredId {
    std::string id;
    std::string afterId;
};

/// Uploaded material and all entity identities reserved for one frozen
/// capture. `captureId` binds this row to the recovery run, so vector order is
/// not an implicit association.
///
/// Overwrite uses one `clips` row per frozen pass and leaves the container,
/// take and comp fields empty. Layers uses one new container, one `takes` row
/// per pass and one `compSegments` row per final non-overlapping comp piece.
struct RecordingCommitCaptureInput {
    std::string captureId;
    AssetRef uploadedAsset;

    std::string containerClipId;
    std::string containerClipAfterId;
    std::vector<RecordingCommitAnchoredId> clips;
    std::vector<RecordingCommitAnchoredId> takes;
    std::vector<RecordingCommitAnchoredId> compSegments;
};

/// Authoritative proof that the frozen recording operation was not committed
/// at an exact project head. Recovery callers obtain this only from
/// `GET /v1/desktop/projects/{projectID}/ops/{opID}` with `found: false`, then
/// materialize the snapshot at `head_seq` and acquire fresh current leases.
/// It must never be inferred from a snapshot's best-effort operation-id set.
struct RecordingCommitOperationAbsenceProof {
    std::string operationId;
    std::uint64_t observedServerSequence = 0;
};

struct RecordingCommitPlanInput {
    CommandMeta meta;
    std::vector<RecordingLeaseClaim> leases;
    std::vector<RecordingCommitCaptureInput> captures;
    std::optional<RecordingCommitOperationAbsenceProof> operationAbsenceProof;
};

/// Facts available before an upload starts. Uploaded assets and the stable
/// document entity ids are deliberately absent, so callers can fail unsafe or
/// over-budget recordings without first publishing their WAV blobs.
struct RecordingCommitPreflightInput {
    std::uint64_t baseServerSequence = 0;
    std::vector<RecordingLeaseClaim> leases;
    std::optional<RecordingCommitOperationAbsenceProof> operationAbsenceProof;
};

enum class RecordingCommitPlanCode : std::uint8_t {
    Planned,
    InvalidInput,
    UnsafeRecovery,
    SnapshotConflict,
    UnsupportedSemantics,
};

struct RecordingCommitPreflightResult {
    RecordingCommitPlanCode code = RecordingCommitPlanCode::InvalidInput;
    std::string message;

    bool ready() const noexcept {
        return code == RecordingCommitPlanCode::Planned;
    }
    explicit operator bool() const noexcept { return ready(); }
};

struct RecordingCommitPlanResult {
    RecordingCommitPlanCode code = RecordingCommitPlanCode::InvalidInput;
    std::string message;
    std::optional<ProjectCommand> command;

    bool planned() const noexcept {
        return code == RecordingCommitPlanCode::Planned && command.has_value();
    }
    explicit operator bool() const noexcept { return planned(); }
};

/// Pure recording landing planner. It reads only immutable shared/recovery
/// data and caller-provided identities. It performs no engine, filesystem,
/// network, upload or UUID work.
class RecordingCommitPlanner {
public:
    /// Exact number of comp-segment identities required for a layered
    /// capture. This lets the durable uploader reserve deterministic ids
    /// without duplicating the planner's overlap algorithm.
    static std::optional<std::size_t> requiredCompSegmentCount(
        const recovery::CloudRecordingCapture& capture);

    /// Pure pre-upload gate shared by the final planner. It validates exact
    /// snapshot head, frozen recovery safety, optional live lease bindings,
    /// bounded pass/command cost and target-range conflicts. An empty lease
    /// list is the v3 new-clip path; the final command validator guarantees
    /// that it cannot target an existing clip/take container.
    static RecordingCommitPreflightResult preflight(
        const SharedProjectDocument& snapshot,
        const recovery::CloudRecordingRecoveryRun& frozenRun,
        const RecordingCommitPreflightInput& input);

    static RecordingCommitPlanResult plan(
        const SharedProjectDocument& snapshot,
        const recovery::CloudRecordingRecoveryRun& frozenRun,
        const RecordingCommitPlanInput& input);
};

} // namespace daw::collab
