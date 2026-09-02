#pragma once

#include "CloudProjectClient.hpp"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVector>

#include <functional>

class QTimer;

namespace collab {

enum class RecordingLeaseState : quint8 {
    Idle,
    Acquiring,
    Held,
    Releasing,
    Lost,
};

/// Narrow asynchronous boundary used by RecordingLeaseCoordinator. Production
/// code uses the CloudProjectClient adapter; tests can inject a deterministic
/// port without constructing a network stack.
class RecordingLeasePort : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~RecordingLeasePort() override = default;

    virtual quint64 acquireRecordingLease(const QString& projectId,
                                           const QString& sessionId,
                                           const QString& trackId,
                                           int ttlSeconds) = 0;
    virtual quint64 renewRecordingLease(const QString& projectId,
                                         const QString& sessionId,
                                         const QString& leaseId,
                                         int ttlSeconds) = 0;
    virtual quint64 releaseRecordingLease(const QString& projectId,
                                           const QString& sessionId,
                                           const QString& leaseId) = 0;
    virtual bool cancel(quint64 requestId) = 0;

signals:
    void leaseReceived(quint64 requestId, collab::CloudRequestKind kind,
                       const collab::CloudProjectTrackLease& lease);
    void operationCompleted(quint64 requestId,
                            collab::CloudRequestKind kind,
                            const QString& resourceId);
    void requestFailed(quint64 requestId, collab::CloudRequestKind kind,
                       const collab::CloudClientError& error);
    void unavailable();
};

/// Owns one atomic set of exclusive recording leases. Once lease ownership is
/// uncertain it remains Lost and retains the last known leases for recovery;
/// callers must explicitly release/reset before another acquisition.
class RecordingLeaseCoordinator final : public QObject {
    Q_OBJECT
public:
    using NowProvider = std::function<QDateTime()>;

    explicit RecordingLeaseCoordinator(CloudProjectClient* client,
                                       QObject* parent = nullptr);
    explicit RecordingLeaseCoordinator(RecordingLeasePort* port,
                                       QObject* parent = nullptr);
    RecordingLeaseCoordinator(RecordingLeasePort* port,
                              NowProvider nowProvider,
                              QObject* parent = nullptr);
    ~RecordingLeaseCoordinator() override;

    RecordingLeaseState state() const { return m_state; }
    QString projectId() const { return m_projectId; }
    QString sessionId() const { return m_sessionId; }
    int ttlSeconds() const { return m_ttlSeconds; }
    QDateTime nextRenewalAt() const { return m_nextRenewalAt; }
    QVector<CloudProjectTrackLease> leases() const;

    /// Begins a parallel, all-or-nothing acquisition. Inputs must already be
    /// canonical lower-case UUIDs; the set must contain 1..8 unique tracks.
    bool acquire(const QString& projectId, const QString& sessionId,
                 CloudProjectRole role, CloudSessionStatus sessionStatus,
                 const QStringList& trackIds, int ttlSeconds = 30);

    /// Best-effort release used by recording Stop. Release failures do not
    /// keep local ownership alive: server TTL remains the final safety net.
    void releaseAll();

    /// Call before switching the bound cloud session. A different or empty
    /// context invalidates pending work and releases all known leases.
    void handleSessionChanged(const QString& projectId,
                              const QString& sessionId);

    /// Call before/while clearing account credentials. No retry is attempted.
    void handleLogout();

    /// Explicit acknowledgement of Lost. Known leases are released first when
    /// the port is available; otherwise recovery metadata is locally cleared.
    void resetAfterLoss();

signals:
    void stateChanged(collab::RecordingLeaseState state);
    void leasesAcquired(
        const QVector<collab::CloudProjectTrackLease>& leases);
    void acquisitionFailed(const collab::CloudClientError& error);
    void leaseLost(
        const collab::CloudClientError& error,
        const QVector<collab::CloudProjectTrackLease>& recoveryLeases);
    void releaseFinished();

private:
    enum class RequestOperation : quint8 { Acquire, Renew, Release };

    struct RequestContext {
        RequestOperation operation = RequestOperation::Acquire;
        quint64 generation = 0;
        QString projectId;
        QString sessionId;
        QString trackId;
        QString leaseId;
    };

    struct DeferredEvent {
        enum class Type : quint8 { Lease, Completed, Failed };
        Type type = Type::Failed;
        quint64 requestId = 0;
        CloudRequestKind kind = CloudRequestKind::AcquireRecordingLease;
        CloudProjectTrackLease lease;
        QString resourceId;
        CloudClientError error;
    };

    void initialize(RecordingLeasePort* port, NowProvider nowProvider);
    quint64 dispatch(const RequestContext& context,
                     const std::function<quint64()>& request);
    void drainDeferredEvents();
    void handleLeaseReceived(quint64 requestId, CloudRequestKind kind,
                             const CloudProjectTrackLease& lease);
    void handleOperationCompleted(quint64 requestId, CloudRequestKind kind,
                                  const QString& resourceId);
    void handleRequestFailed(quint64 requestId, CloudRequestKind kind,
                             const CloudClientError& error);
    void handlePortUnavailable();

    void finishAcquireIfReady();
    void failAcquire(const CloudClientError& error);
    void beginRelease(bool fromReset = false);
    void finishReleaseIfReady();
    void renewNow();
    void finishRenewIfReady();
    void scheduleHeartbeat();
    void scheduleExpiryCheck();
    void checkExpiry();
    void transitionLost(const CloudClientError& error);
    void cancelPending();
    void clearContext();
    void setState(RecordingLeaseState state);
    QDateTime nowUtc() const;

    static bool isCanonicalUuid(const QString& value);
    static bool requestKindMatches(RequestOperation operation,
                                   CloudRequestKind kind);
    static CloudClientError localError(CloudClientErrorCode code,
                                       const QString& message,
                                       bool retryable = false);

    QPointer<RecordingLeasePort> m_port;
    bool m_portAvailable = false;
    QTimer* m_renewTimer = nullptr;
    QTimer* m_expiryTimer = nullptr;
    NowProvider m_nowProvider;
    RecordingLeaseState m_state = RecordingLeaseState::Idle;
    quint64 m_generation = 0;
    int m_ttlSeconds = 30;
    QString m_projectId;
    QString m_sessionId;
    QStringList m_requestedTrackIds;
    QHash<QString, CloudProjectTrackLease> m_leasesByTrack;
    QHash<quint64, RequestContext> m_requests;
    int m_dispatchDepth = 0;
    QVector<DeferredEvent> m_deferredEvents;
    bool m_acquireDispatchComplete = false;
    bool m_renewDispatchComplete = false;
    bool m_releaseDispatchComplete = false;
    QDateTime m_nextRenewalAt;

    friend bool checkRecordingLeaseCoordinatorForTest(QString* error);
};

bool checkRecordingLeaseCoordinatorForTest(QString* error = nullptr);

} // namespace collab

Q_DECLARE_METATYPE(collab::RecordingLeaseState)
