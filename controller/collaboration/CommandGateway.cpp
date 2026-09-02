#include "collaboration/CommandGateway.hpp"

#include "collaboration/CommandJson.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace daw::collab {
namespace {

/// The canonical wire codec excludes server-assigned sequence/actor metadata
/// but includes the complete command body, preconditions, base sequence and
/// transaction id.  With one pending command and an ordered confirmed base,
/// an exact wire match proves that replay materializes the same ProjectModel;
/// comparing the op id alone would not provide that guarantee.
bool sameOptimisticMutation(const ProjectCommand& pending,
                            const ProjectCommand& committed) {
    // Qt's compact JSON bridge is allowed to spell an integral-valued double
    // such as 135.0 as 135. nlohmann's structural equality deliberately
    // compares numeric values across integer/float storage, whereas comparing
    // dumped strings would mistake that lossless wire round-trip for a changed
    // acknowledgement and needlessly project the same local edit twice.
    return projectCommandToJson(pending) ==
           projectCommandToJson(committed);
}

} // namespace

CommandGateway::CommandGateway(SharedProjectDocument initial,
                               ProjectProjectionAdapter* adapter)
    : m_confirmed(std::move(initial)),
      m_optimistic(m_confirmed),
      m_adapter(adapter) {}

void CommandGateway::notify(const ChangeImpact& impact, ProjectionOrigin origin) {
    if (m_adapter) m_adapter->projectChanged(m_optimistic, impact, origin);
}

GatewayUpdate CommandGateway::submit(ProjectCommand command) {
    GatewayUpdate update;
    std::string idError;
    if (!commandHasValidIds(command, &idError)) {
        update.code = GatewayCode::Rejected;
        update.apply.code = ApplyCode::InvalidCommand;
        update.apply.message = std::move(idError);
        return update;
    }
    command.meta.serverSequence = 0;
    if (command.meta.baseServerSequence == 0)
        command.meta.baseServerSequence = m_confirmed.confirmedSequence;
    ApplyResult applied = ProjectReducer::apply(m_optimistic, command);
    update.apply = applied;
    if (!applied.accepted()) {
        update.code = GatewayCode::Rejected;
        return update;
    }
    if (applied.code == ApplyCode::Duplicate) {
        update.code = GatewayCode::Duplicate;
        return update;
    }
    m_pending.push_back(std::move(command));
    update.code = GatewayCode::Accepted;
    notify(applied.impact, ProjectionOrigin::OptimisticLocal);
    return update;
}

ChangeImpact CommandGateway::rebasePending(std::vector<std::string>& dropped) {
    m_optimistic = m_confirmed;
    std::vector<ProjectCommand> kept;
    kept.reserve(m_pending.size());
    ChangeImpact impact;
    impact.fullProjection = true;
    impact.documentChanged = true;
    for (ProjectCommand& pending : m_pending) {
        ApplyResult replayed = ProjectReducer::apply(m_optimistic, pending);
        if (!replayed.accepted()) {
            dropped.push_back(pending.meta.operationId);
            continue;
        }
        impact.merge(replayed.impact);
        kept.push_back(std::move(pending));
    }
    m_pending = std::move(kept);
    return impact;
}

GatewayUpdate CommandGateway::receiveConfirmed(ProjectCommand command) {
    GatewayUpdate update;
    std::string idError;
    if (!commandHasValidIds(command, &idError)) {
        update.code = GatewayCode::Rejected;
        update.apply.code = ApplyCode::InvalidCommand;
        update.apply.message = std::move(idError);
        return update;
    }
    const std::uint64_t sequence = command.meta.serverSequence;
    if (sequence == 0 || sequence > m_confirmed.confirmedSequence + 1) {
        update.code = GatewayCode::SequenceGap;
        update.apply.code = ApplyCode::PreconditionsFailed;
        update.apply.message = "confirmed command sequence gap";
        return update;
    }
    if (sequence <= m_confirmed.confirmedSequence) {
        if (m_confirmed.appliedOperationIds.contains(command.meta.operationId)) {
            update.code = GatewayCode::Duplicate;
            update.apply.code = ApplyCode::Duplicate;
            return update;
        }
        const bool acknowledgesPending = std::any_of(
            m_pending.begin(), m_pending.end(),
            [&](const ProjectCommand& pending) {
                return pending.meta.operationId == command.meta.operationId;
            });
        if (acknowledgesPending) {
            std::erase_if(m_pending, [&](const ProjectCommand& pending) {
                return pending.meta.operationId == command.meta.operationId;
            });
            ChangeImpact impact =
                rebasePending(update.droppedPendingOperationIds);
            update.code = GatewayCode::Duplicate;
            update.apply.code = ApplyCode::Duplicate;
            update.apply.impact = impact;
            notify(impact, ProjectionOrigin::Rebase);
            return update;
        }
        update.code = GatewayCode::SequenceGap;
        update.apply.code = ApplyCode::PreconditionsFailed;
        update.apply.message = "stale sequence carries an unknown operation";
        return update;
    }

    const auto matchingPending = std::find_if(
        m_pending.begin(), m_pending.end(), [&](const ProjectCommand& pending) {
            return pending.meta.operationId == command.meta.operationId;
        });
    const bool wasPending = matchingPending != m_pending.end();
    // Only the sole optimistic command is eligible for the no-rematerialize
    // fast path. Matching op ids are not sufficient: the complete canonical
    // mutation must match before reducer metadata can advance silently.
    const bool metadataOnlyAcknowledgement =
        wasPending && m_pending.size() == 1 &&
        sameOptimisticMutation(*matchingPending, command);

    ApplyResult confirmed = ProjectReducer::apply(m_confirmed, command);
    if (!confirmed.accepted()) {
        update.code = GatewayCode::Rejected;
        update.apply = std::move(confirmed);
        return update;
    }
    m_confirmed.confirmedSequence = sequence;
    std::erase_if(m_pending, [&](const ProjectCommand& pending) {
        return pending.meta.operationId == command.meta.operationId;
    });
    ChangeImpact impact = confirmed.impact;
    impact.merge(rebasePending(update.droppedPendingOperationIds));
    update.code = GatewayCode::Accepted;
    update.apply = std::move(confirmed);
    const ProjectionOrigin origin = !m_pending.empty()
                                        ? ProjectionOrigin::Rebase
                                        : (wasPending
                                               ? ProjectionOrigin::ConfirmedLocalAck
                                               : ProjectionOrigin::ConfirmedRemote);
    if (!metadataOnlyAcknowledgement || !m_pending.empty())
        notify(impact, origin);
    return update;
}

GatewayUpdate CommandGateway::rejectPending(const std::string& operationId,
                                            std::string reason) {
    GatewayUpdate update;
    const auto found = std::find_if(
        m_pending.begin(), m_pending.end(), [&](const ProjectCommand& command) {
            return command.meta.operationId == operationId;
        });
    if (found == m_pending.end()) {
        update.code = GatewayCode::PendingNotFound;
        update.apply.code = ApplyCode::MissingEntity;
        update.apply.message = "pending operation not found";
        return update;
    }
    m_pending.erase(found);
    ChangeImpact impact = rebasePending(update.droppedPendingOperationIds);
    update.code = GatewayCode::Accepted;
    update.apply.code = ApplyCode::Applied;
    update.apply.message = std::move(reason);
    update.apply.impact = impact;
    notify(impact, ProjectionOrigin::Rebase);
    return update;
}

GatewayUpdate CommandGateway::replaceConfirmed(SharedProjectDocument snapshot,
                                               std::uint64_t serverSequence) {
    snapshot.confirmedSequence = serverSequence;
    m_confirmed = std::move(snapshot);
    GatewayUpdate update;
    ChangeImpact impact = rebasePending(update.droppedPendingOperationIds);
    impact.fullProjection = true;
    update.code = GatewayCode::Accepted;
    update.apply.code = ApplyCode::Applied;
    update.apply.impact = impact;
    notify(impact, ProjectionOrigin::Snapshot);
    return update;
}

} // namespace daw::collab
