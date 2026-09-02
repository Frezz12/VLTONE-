#pragma once

#include "CollaborationTypes.hpp"
#include "collaboration/CommandGateway.hpp"
#include "collaboration/ConditionalUndo.hpp"
#include "collaboration/SharedMutationSink.hpp"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace collab {

class CollaborationService;

enum class LocalOperationCode {
    Submitted,
    Duplicate,
    Rejected,
    TransportUnavailable,
    ResyncRequired,
};

struct LocalOperationResult {
    LocalOperationCode code = LocalOperationCode::Rejected;
    QString operationId;
    QString message;

    bool submitted() const noexcept {
        return code == LocalOperationCode::Submitted;
    }
};

enum class DurableOperationWatchCode : std::uint8_t {
    Watching,
    AlreadyObserved,
    InvalidOperationId,
    ProjectUnbound,
    CapacityExceeded,
};

/// Synchronous result of registering a recovery/cleanup interest in one
/// operation. AlreadyObserved is the race-free query path: the signal remains
/// once-per-project, while a late watcher still receives proof from the
/// current verified confirmed lineage. For that query path, a snapshot-style
/// sequence is an inclusive upper bound rather than the operation's exact seq.
struct DurableOperationWatchResult {
    DurableOperationWatchCode code =
        DurableOperationWatchCode::InvalidOperationId;
    quint64 serverSequence = 0;
    bool viaVerifiedSnapshot = false;

    bool accepted() const noexcept {
        return code == DurableOperationWatchCode::Watching ||
               code == DurableOperationWatchCode::AlreadyObserved;
    }
};

/// Routes the strict Qt WebSocket envelope to the framework-independent
/// CommandGateway. It owns no EngineController; document application and
/// optimistic replay stay inside CommandGateway, while this bridge owns the
/// current actor's conditional cloud undo history.
class CollaborationCommandBridge final : public QObject,
                                         public daw::collab::SharedMutationSink {
    Q_OBJECT
public:
    using OperationSender = std::function<bool(const QJsonObject& command)>;
    using AvailabilityCheck = std::function<bool()>;
    using ProjectIdProvider = std::function<QString()>;

    CollaborationCommandBridge(CollaborationService* service,
                               daw::collab::CommandGateway* gateway,
                               QObject* parent = nullptr);

    /// Injection seam used by deterministic client tests. Production uses the
    /// CollaborationService constructor above.
    CollaborationCommandBridge(daw::collab::CommandGateway* gateway,
                               OperationSender sender,
                               AvailabilityCheck available,
                               QObject* parent = nullptr);
    CollaborationCommandBridge(daw::collab::CommandGateway* gateway,
                               OperationSender sender,
                               AvailabilityCheck available,
                               ProjectIdProvider projectId,
                               QObject* parent = nullptr);

    /// True means this document is cloud-bound even when its room is offline,
    /// read-only or resyncing. Callers must never fall through to a local
    /// mutation/legacy undo while this is true.
    bool handlesCloudBinding() override;
    daw::collab::SharedMutationResult submit(
        daw::collab::SharedMutationRequest request) override;
    qsizetype pendingOperationCount() const;
    QVector<daw::collab::ProjectCommand> journaledOperations(
        const QString& projectId) const;
    qsizetype journalEntryCount(const QString& projectId) const;
    bool retireJournaledOperation(const QString& projectId,
                                  const QString& operationId);
    bool canUndo();
    bool canRedo();
    bool requestUndo();
    bool requestRedo();

    daw::collab::SharedMutationResult setTimeSignature(
        int numerator, int denominator) override;
    daw::collab::SharedMutationResult setProjectKey(
        int root, std::string_view scaleId) override;
    daw::collab::SharedMutationResult setAiInstructions(
        std::string_view text) override;
    daw::collab::SharedMutationResult renameTrack(
        std::string_view trackId, std::string_view name) override;
    daw::collab::SharedMutationResult setTrackMuted(
        std::string_view trackId, bool muted) override;
    daw::collab::SharedMutationResult setTracksMuted(
        std::span<const std::string> trackIds, bool muted) override;
    daw::collab::SharedMutationResult clearAllMutes(
        std::span<const std::string> mutedTrackIds) override;

    LocalOperationResult submitLocal(daw::collab::ProjectCommand command);
    /// Replays a command recovered from the durable pending journal while
    /// normal edits remain blocked. The original opId is preserved.
    LocalOperationResult resubmitJournaled(
        daw::collab::ProjectCommand command);
    /// Watches one canonical operation id for proof supplied by the current or
    /// a future verified confirmed state. The bounded watch is scoped to the
    /// current project binding and is retired once observed. A live commit is
    /// observed regardless of whether it was watched. AlreadyObserved returns
    /// the existing proof synchronously and does not re-emit the signal. An
    /// explicit rejection or pending-queue drop retires an active watch through
    /// operationDurabilityFailed; callers may register a new watch before an
    /// intentional retry of the same operation id. Snapshot non-membership is
    /// not a negative result: after restart, recovery must use the REST opId
    /// lookup before deciding that it is safe to submit again.
    DurableOperationWatchResult watchDurableOperation(
        const QString& operationId);
    daw::collab::GatewayUpdate replaceConfirmedSnapshot(
        daw::collab::SharedProjectDocument snapshot,
        quint64 serverSequence);
    /// Returns a synchronous immutable copy of the reducer's confirmed state
    /// only when it is exactly the requested verified sequence. Optimistic
    /// pending commands are intentionally never visible through this API.
    std::optional<daw::collab::SharedProjectDocument> confirmedSnapshotAt(
        quint64 serverSequence) const;
    quint64 confirmedServerSequence() const noexcept;
    bool resyncPending() const noexcept { return m_resyncPending; }
    /// Latches the command path while an external bootstrap coordinator proves
    /// a newer canonical generation. Incoming committed ops are bounded and
    /// replayed after replacement instead of being discarded.
    void requireResync(const QString& safeReason);

public slots:
    void receiveDurableEnvelope(const collab::WireEnvelope& envelope);
    /// Direct hookup target for EngineProjectProjectionAdapter::projectionFailed.
    /// The engine error itself is intentionally not propagated across the
    /// collaboration/UI boundary; the bridge latches a generic verified-resync
    /// requirement and CollaborationService becomes non-writable immediately.
    void handleProjectionFailure(const QString& projectionError);

signals:
    void resyncRequired(quint64 expectedServerSequence,
                        quint64 receivedServerSequence,
                        const QString& safeReason);
    void operationCommitted(const QString& operationId,
                            quint64 serverSequence,
                            bool localAcknowledgement);
    /// A durable-observation boundary for cleanup/recovery workflows. It is
    /// emitted once per recently observed operation in the current project.
    /// Accepted remote commits are observed as well as local acknowledgements.
    /// `serverSequence` is the operation's exact sequence for an
    /// `op.committed` envelope and the verified snapshot head (an inclusive
    /// upper bound) when `viaVerifiedSnapshot` is true.
    void operationDurablyObserved(const QString& operationId,
                                  quint64 serverSequence,
                                  bool viaVerifiedSnapshot);
    /// Terminal failure for one active durable watch. The watch is removed
    /// before this signal is emitted, so rejection rollback and duplicate
    /// rejection delivery cannot consume capacity or notify cleanup twice.
    /// `code` is the validated server rejection code, `pending_dropped` when
    /// the optimistic queue discarded the operation without an explicit
    /// op.rejected envelope, or `project_binding_changed` when Leave/Open or
    /// service teardown made the watched project no longer current.
    /// `safeMessage` contains no command/project payload.
    void operationDurabilityFailed(const QString& operationId,
                                   const QString& code,
                                   const QString& safeMessage);
    void operationRejected(const QString& operationId,
                           const QString& code,
                           const QString& safeMessage);
    void pendingOperationsDropped(const QStringList& operationIds);
    void protocolWarning(const QString& safeMessage);
    void canUndoChanged(bool available);
    void canRedoChanged(bool available);
    void mutationBlocked(const QString& safeMessage);

private:
    void receiveCommitted(const QJsonObject& payload);
    void receiveRejected(const QJsonObject& payload);
    void markResync(quint64 expected, quint64 received,
                    const QString& safeReason);
    void reportDropped(
        const std::vector<std::string>& operationIds,
        std::string_view durableWatchExclusion = {});
    void deferCommitted(const collab::WireEnvelope& envelope);
    void drainDeferredCommitted();
    void refreshProjectBinding();
    void applyProjectBinding(const QString& projectId);
    void clearPendingBookkeeping();
    void forgetPending(const std::string& operationId);
    bool rememberDurableOperation(const std::string& operationId,
                                  quint64 serverSequence,
                                  bool viaVerifiedSnapshot);
    void observeDurableOperation(const std::string& operationId,
                                 quint64 serverSequence,
                                 bool viaVerifiedSnapshot);
    void failDurableOperationWatch(const std::string& operationId,
                                   const QString& code,
                                   const QString& safeMessage);
    void clearDurableObservations();
    void emitHistoryAvailability();
    bool hasPendingHistoryTransition() const;
    daw::collab::CommandMeta freshMeta(bool transaction = true) const;
    daw::collab::SharedMutationResult submitShared(
        daw::collab::CommandBody body, std::string label,
        std::optional<std::string> transactionId = std::nullopt);
    daw::collab::SharedMutationResult submitMuteBatch(
        std::span<const std::string> trackIds, bool muted,
        std::string label);
    bool submitPreparedHistory(bool redo);
    LocalOperationResult submitLocalImpl(
        daw::collab::ProjectCommand command, bool journalRecovery);

    enum class PendingHistoryKind : std::uint8_t { Forward, Undo, Redo };
    struct PendingHistory {
        PendingHistoryKind kind = PendingHistoryKind::Forward;
        daw::collab::ProjectCommand command;
        std::string label;
        std::uint64_t historyToken = 0;
    };

    CollaborationService* m_service = nullptr;
    daw::collab::CommandGateway* m_gateway = nullptr;
    OperationSender m_sender;
    AvailabilityCheck m_available;
    ProjectIdProvider m_projectIdProvider;
    QString m_boundProjectId;
    QString m_verifiedSnapshotProjectId;
    daw::collab::ConditionalUndoHistory m_history;
    std::unordered_map<std::string, PendingHistory> m_pendingHistory;
    bool m_lastCanUndo = false;
    bool m_lastCanRedo = false;
    bool m_resyncPending = false;
    QVector<collab::WireEnvelope> m_deferredCommitted;
    qsizetype m_deferredCommittedBytes = 0;
    bool m_deferredOverflow = false;
    std::unordered_map<std::string, DurableOperationWatchResult>
        m_durableObservations;
    std::deque<std::string> m_durableObservationOrder;
    std::unordered_set<std::string> m_watchedDurableOperationIds;
};

bool checkCollaborationCommandBridgeForTest(QString* error = nullptr);

} // namespace collab
