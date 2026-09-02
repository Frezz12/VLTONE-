#pragma once

#include "collaboration/MutationCapabilityLedger.hpp"
#include "collaboration/ProjectReducer.hpp"

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

constexpr bool commandCoverageComplete() noexcept {
    return mutationCapabilityLedgerIsClassified();
}

#if defined(DAW_ENFORCE_COLLABORATION_RELEASE_GATES)
static_assert(commandCoverageComplete(),
              "collaboration release gate: command/controller coverage is incomplete");
#endif

} // namespace daw::collab
