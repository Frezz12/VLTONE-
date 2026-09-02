#pragma once

#include "collaboration/EngineControllerAdapter.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace daw::collab {

enum class GatewayCode : std::uint8_t {
    Accepted,
    Duplicate,
    Rejected,
    SequenceGap,
    PendingNotFound,
};

struct GatewayUpdate {
    GatewayCode code = GatewayCode::Rejected;
    ApplyResult apply;
    std::vector<std::string> droppedPendingOperationIds;

    bool accepted() const noexcept {
        return code == GatewayCode::Accepted || code == GatewayCode::Duplicate;
    }
};

/// Owns confirmed and optimistic materializations. The server stream advances
/// only the confirmed copy; every change then replays still-pending local
/// commands in original client order.
class CommandGateway {
public:
    explicit CommandGateway(SharedProjectDocument initial = {},
                            ProjectProjectionAdapter* adapter = nullptr);

    GatewayUpdate submit(ProjectCommand command);
    /// Advances the canonical sequence and rebases pending commands. An exact
    /// acknowledgement of the sole optimistic command updates reducer metadata
    /// without notifying the projection adapter because its ProjectModel is
    /// already materialized; every material mismatch still notifies.
    GatewayUpdate receiveConfirmed(ProjectCommand command);
    GatewayUpdate rejectPending(const std::string& operationId,
                                std::string reason = {});
    GatewayUpdate replaceConfirmed(SharedProjectDocument snapshot,
                                   std::uint64_t serverSequence);

    const SharedProjectDocument& confirmed() const noexcept { return m_confirmed; }
    const SharedProjectDocument& optimistic() const noexcept { return m_optimistic; }
    const std::vector<ProjectCommand>& pending() const noexcept { return m_pending; }
    void setAdapter(ProjectProjectionAdapter* adapter) noexcept { m_adapter = adapter; }

private:
    ChangeImpact rebasePending(std::vector<std::string>& dropped);
    void notify(const ChangeImpact& impact, ProjectionOrigin origin);

    SharedProjectDocument m_confirmed;
    SharedProjectDocument m_optimistic;
    std::vector<ProjectCommand> m_pending;
    ProjectProjectionAdapter* m_adapter = nullptr;
};

} // namespace daw::collab
