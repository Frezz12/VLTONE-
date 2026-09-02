#pragma once

#include "collaboration/ProjectReducer.hpp"

#include <array>
#include <string_view>

namespace daw::collab {

enum class ProjectionOrigin : std::uint8_t {
    OptimisticLocal,
    ConfirmedLocalAck,
    ConfirmedRemote,
    Rebase,
    Snapshot,
};

/// Seam for the future EngineController migration. The reducer remains pure;
/// an implementation projects accepted document changes into graph/note/clip
/// sync without generating a second command or an UndoStack entry.
class ProjectProjectionAdapter {
public:
    virtual ~ProjectProjectionAdapter() = default;
    virtual void projectChanged(const SharedProjectDocument& document,
                                const ChangeImpact& impact,
                                ProjectionOrigin origin) = 0;
};

enum class CoverageStatus : std::uint8_t {
    ReducerReady,
    ModelOnly,
    NotStarted,
};

struct CommandCoverageEntry {
    std::string_view family;
    CoverageStatus status;
    std::string_view note;
};

/// Explicit migration ledger: adding a controller adapter path should move one
/// row to ReducerReady and add convergence tests. It prevents a coarse
/// `projectEdited()` fallback from silently claiming unsupported mutations.
inline constexpr std::array kCommandCoverage{
    CommandCoverageEntry{"project-scalars", CoverageStatus::ReducerReady,
                         "name/tempo/time-signature/key/master gain"},
    CommandCoverageEntry{"track-lifecycle-order", CoverageStatus::ReducerReady,
                         "stable ids, afterId, delete tombstones"},
    CommandCoverageEntry{"track-properties", CoverageStatus::ReducerReady,
                         "name/color/volume/pan/mute/mono"},
    CommandCoverageEntry{"batch-conditional-undo", CoverageStatus::ReducerReady,
                         "atomic batch and writer-guarded inverse commands"},
    CommandCoverageEntry{"assets-plugin-compatibility", CoverageStatus::ModelOnly,
                         "v6 model/serialization, no upload/cache implementation"},
    CommandCoverageEntry{"clip-lifecycle-order-properties",
                         CoverageStatus::ReducerReady,
                         "stable ids, afterId, tombstones and scalar properties"},
    CommandCoverageEntry{"controller-lanes-automation-settings",
                         CoverageStatus::ReducerReady,
                         "lane lifecycle/target/default and primary automation settings"},
    CommandCoverageEntry{"take-lifecycle", CoverageStatus::ReducerReady,
                         "cloud AssetRef take add/delete/restore in the model"},
    CommandCoverageEntry{"recording-commit-projection", CoverageStatus::NotStarted,
                         "capture, upload and EngineController adapter required"},
    CommandCoverageEntry{"midi-notes-automation-points",
                         CoverageStatus::ReducerReady,
                         "upsert/delete/restore with stable ids and anchors"},
    CommandCoverageEntry{"comp-segments", CoverageStatus::ReducerReady,
                         "upsert/delete/restore with stable ids and anchors"},
    CommandCoverageEntry{"plugin-slot-state-parameters",
                         CoverageStatus::ReducerReady,
                         "slot lifecycle/order, host properties, state, stable parameters and asset bindings"},
    CommandCoverageEntry{"engine-projection", CoverageStatus::ReducerReady,
                         "gateway materialization transactionally projects graph/model, local state and cached assets"},
    CommandCoverageEntry{"legacy-command-migration", CoverageStatus::NotStarted,
                         "existing UI/AI/controller mutations still bypass CommandGateway and use legacy UndoStack"},
};

constexpr bool commandCoverageComplete() noexcept {
    for (const CommandCoverageEntry& entry : kCommandCoverage) {
        if (entry.status != CoverageStatus::ReducerReady) return false;
    }
    return true;
}

#if defined(DAW_ENFORCE_COLLABORATION_RELEASE_GATES)
static_assert(commandCoverageComplete(),
              "collaboration release gate: command/controller coverage is incomplete");
#endif

} // namespace daw::collab
