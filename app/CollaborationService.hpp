#pragma once

#include "CollaborationTypes.hpp"
#include "LocalSessionState.hpp"
#include "PresenceStore.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QUrl>

namespace account { class Service; }

namespace collab {

/// UI-thread collaboration state and versioned wire boundary. It deliberately
/// owns no credentials and opens no sockets. CollaborationTransport forwards
/// `outboundTextMessage` and feeds validated text through
/// `receiveTrustedTextMessage`.
class CollaborationService final : public QObject {
    Q_OBJECT
public:
    explicit CollaborationService(account::Service* account,
                                  QObject* parent = nullptr);

    CollaborationState state() const { return m_state; }
    QString stateDetail() const { return m_stateDetail; }
    QString projectId() const { return m_projectId; }
    QString sessionId() const { return m_sessionId; }
    QString localParticipantId() const {
        return m_presenceStore.localParticipantId();
    }
    QString accountUserId() const;
    QString hostParticipantId() const {
        return m_localSessionState.hostParticipantId();
    }
    bool isOnline() const {
        return m_transportConnected &&
               (m_state == CollaborationState::Synced ||
                m_state == CollaborationState::ReadOnly);
    }
    PresenceStore* presenceStore() { return &m_presenceStore; }
    LocalSessionState* localSessionState() { return &m_localSessionState; }

    /// Opening a local project never uploads it or connects. A cloud flow must
    /// explicitly supply the server project id.
    void setProjectId(const QString& projectId, bool requestConnection = true);
    void clearProject();
    void reconnectNow();
    void disconnectFromProject();

    /// Records the exact sequence materialized by a verified REST bootstrap.
    /// The WebSocket hello advertises this value and a welcome at any other
    /// head remains resync-blocked. Callers must install the document in the
    /// CommandGateway before calling trustedResyncCompleted().
    bool installVerifiedBootstrapSequence(const QString& projectId,
                                          quint64 serverSequence);
    /// Installs the canonical SHA-256 for the exact materialized sequence.
    /// The digest is cleared when a newer committed operation is projected.
    bool installVerifiedBootstrapState(const QString& projectId,
                                       quint64 serverSequence,
                                       const QString& stateHash);
    /// Advances the reconnect cursor after one strictly-next committed op.
    bool advanceMaterializedSequence(const QString& projectId,
                                     quint64 serverSequence);
    quint64 bootstrapServerSequence() const noexcept {
        return m_bootstrapServerSequence;
    }
    QString bootstrapStateHash() const { return m_bootstrapStateHash; }

    /// Narrow boundary for the trusted transport. These methods carry no
    /// account token, file path or widget content.
    void trustedTransportConnected();
    void trustedTransportDisconnected(const QString& safeReason = {});
    /// Marks a non-retryable transport/configuration failure. A user-triggered
    /// reconnect clears this latch and requests fresh authorization.
    void trustedTransportUnavailable(const QString& safeReason);
    /// Blocks every durable submit path while a verified snapshot/log replay
    /// is required. `readOnly` is sticky for the current room welcome and a
    /// conflict is never downgraded by a later duplicate resync request.
    void trustedResyncRequired(bool conflict, bool readOnly,
                               const QString& safeReason = {});
    /// Completes a verified bootstrap without reconnecting the healthy socket.
    /// Refuses stale callbacks unless the same cloud project and transport are
    /// still connected.
    bool trustedResyncCompleted();
    /// Exposes a verified cached cloud generation without enabling presence or
    /// mutation submits. A user-triggered reconnect leaves this mode.
    void trustedOfflineProjectOpened();
    void receiveTrustedTextMessage(const QString& message);

    bool canSubmitOperations() const;
    /// Recovery commands are the only durable writes allowed while the
    /// bootstrap coordinator is resolving the account-scoped pending journal.
    bool canSubmitRecoveryOperations() const;
    void setPendingRecoveryBlocked(bool blocked);
    bool pendingRecoveryBlocked() const noexcept {
        return m_pendingRecoveryBlocked;
    }
    /// Sends one already validated locked project-command object. The command
    /// bridge owns typed validation and optimistic state; this service only
    /// wraps it in the versioned op.submit envelope.
    bool submitOperation(const QJsonObject& command);
    bool submitRecoveryOperation(const QJsonObject& command);
    void sendPresence(const PresencePacket& packet);
    void sendTransport(const TransportFrame& frame);
    bool sendSnapshotHash();

signals:
    void stateChanged(collab::CollaborationState state,
                      const QString& detail);
    void projectChanged(const QString& projectId);
    void roomAuthorizationRequired(const QString& projectId,
                                   const QUrl& endpoint);
    void outboundTextMessage(const QString& message);
    void protocolWarning(const QString& message);
    void durableEnvelopeReceived(const collab::WireEnvelope& envelope);
    void resyncRequired(const QJsonObject& payload);
    /// Requests canonical hashing of the exact confirmed reducer document.
    /// The coordinator performs serialization off the UI/audio threads and
    /// installs the digest only if this round is still current.
    void hashRoundRequested(const QString& roundId,
                            const QString& sessionId,
                            quint64 serverSequence,
                            qint64 deadlineMs);
    /// Emitted only after the server-control payload has been strictly
    /// validated against this socket's current session and local host identity.
    void snapshotRequested(const collab::SnapshotRequest& request);
    /// Invalidates work bound to a previous room or host assignment.
    void roomIdentityChanged(const QString& sessionId,
                             const QString& localParticipantId,
                             const QString& hostParticipantId);
    /// Authenticated room lifecycle boundaries. The session remains connected
    /// throughout `liveSessionEnding` so the selected host can upload the
    /// final snapshot; teardown occurs only at `liveSessionEnded`.
    void liveSessionEnding(const QString& sessionId);
    void liveSessionEnded(const QString& sessionId);

private:
    void refreshAccountState();
    void requestTrustedTransport();
    void setState(CollaborationState state, const QString& detail = {});
    QUrl collaborationUrl() const;
    bool sendEnvelope(WireType type, const QJsonObject& payload,
                      bool ephemeral = false);
    void handleEnvelope(const WireEnvelope& envelope);
    bool acceptHashRound(const QJsonObject& payload);
    void requestOrSendRoundHash();

    account::Service* m_account = nullptr;
    PresenceStore m_presenceStore;
    LocalSessionState m_localSessionState;
    CollaborationState m_state = CollaborationState::LocalOnly;
    QString m_stateDetail;
    QString m_projectId;
    QString m_sessionId;
    quint64 m_bootstrapServerSequence = 0;
    QString m_bootstrapStateHash;
    QString m_hashRoundId;
    QString m_hashRoundSessionId;
    quint64 m_hashRoundServerSequence = 0;
    qint64 m_hashRoundDeadlineMs = 0;
    quint64 m_ephemeralSequence = 0;
    int m_maxMessageBytes = 1024 * 1024;
    bool m_shouldConnect = false;
    bool m_transportConnected = false;
    bool m_sessionReadOnly = false;
    bool m_pendingRecoveryBlocked = false;
    bool m_resyncPending = false;
    bool m_authorizationRequested = false;
    bool m_transportSent = false;
    bool m_lastTransportPlaying = false;
    QElapsedTimer m_monotonicClock;
    QElapsedTimer m_lastTransportSent;

    friend bool checkCollaborationCommandBridgeForTest(QString* error);
    friend bool checkSnapshotRequestUploaderForTest(QString* error);
};

/// Non-network self-test for the final AsyncAPI presence whitelist, including
/// the surface-only coarse policy.
bool checkCollaborationPresenceSafetyForTest(QString* error = nullptr);

} // namespace collab
