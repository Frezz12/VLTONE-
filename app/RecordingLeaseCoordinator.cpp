#include "RecordingLeaseCoordinator.hpp"

#include <QSet>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace collab {
namespace {

constexpr int kMinimumLeaseTtlSeconds = 5;
constexpr int kMaximumLeaseTtlSeconds = 120;
constexpr int kMaximumRecordingTracks = 8;
constexpr qint64 kMaximumTimerIntervalMs = 60'000;

bool isLeaseRequestKind(CloudRequestKind kind) {
    return kind == CloudRequestKind::AcquireRecordingLease ||
           kind == CloudRequestKind::RenewRecordingLease ||
           kind == CloudRequestKind::ReleaseRecordingLease;
}

class CloudProjectRecordingLeasePort final : public RecordingLeasePort {
public:
    CloudProjectRecordingLeasePort(CloudProjectClient* client,
                                   QObject* parent)
        : RecordingLeasePort(parent), m_client(client) {
        if (!client) return;
        connect(client, &CloudProjectClient::leaseReceived, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudProjectTrackLease& lease) {
                    if (isLeaseRequestKind(kind))
                        emit leaseReceived(requestId, kind, lease);
                });
        connect(client, &CloudProjectClient::operationCompleted, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const QString& resourceId) {
                    if (kind == CloudRequestKind::ReleaseRecordingLease)
                        emit operationCompleted(requestId, kind, resourceId);
                });
        connect(client, &CloudProjectClient::requestFailed, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudClientError& error) {
                    if (isLeaseRequestKind(kind))
                        emit requestFailed(requestId, kind, error);
                });
        connect(client, &QObject::destroyed, this, [this] {
            m_client = nullptr;
            emit unavailable();
        });
    }

    bool available() const { return !m_client.isNull(); }

    quint64 acquireRecordingLease(const QString& projectId,
                                   const QString& sessionId,
                                   const QString& trackId,
                                   int ttlSeconds) override {
        return m_client
            ? m_client->acquireRecordingLease(projectId, sessionId, trackId,
                                              ttlSeconds)
            : 0;
    }

    quint64 renewRecordingLease(const QString& projectId,
                                 const QString& sessionId,
                                 const QString& leaseId,
                                 int ttlSeconds) override {
        return m_client
            ? m_client->renewRecordingLease(projectId, sessionId, leaseId,
                                            ttlSeconds)
            : 0;
    }

    quint64 releaseRecordingLease(const QString& projectId,
                                   const QString& sessionId,
                                   const QString& leaseId) override {
        return m_client
            ? m_client->releaseRecordingLease(projectId, sessionId, leaseId)
            : 0;
    }

    bool cancel(quint64 requestId) override {
        return m_client && m_client->cancel(requestId);
    }

private:
    QPointer<CloudProjectClient> m_client;
};

bool leaseTimesValid(const CloudProjectTrackLease& lease) {
    return lease.acquiredAt.isValid() && lease.renewedAt.isValid() &&
           lease.expiresAt.isValid() &&
           lease.acquiredAt <= lease.renewedAt &&
           lease.renewedAt < lease.expiresAt;
}

} // namespace

RecordingLeaseCoordinator::RecordingLeaseCoordinator(
    CloudProjectClient* client, QObject* parent)
    : QObject(parent) {
    auto* adapter = new CloudProjectRecordingLeasePort(client, this);
    initialize(adapter, {});
    if (!adapter->available()) handlePortUnavailable();
}

RecordingLeaseCoordinator::RecordingLeaseCoordinator(
    RecordingLeasePort* port, QObject* parent)
    : QObject(parent) {
    initialize(port, {});
}

RecordingLeaseCoordinator::RecordingLeaseCoordinator(
    RecordingLeasePort* port, NowProvider nowProvider, QObject* parent)
    : QObject(parent) {
    initialize(port, std::move(nowProvider));
}

RecordingLeaseCoordinator::~RecordingLeaseCoordinator() {
    if (m_renewTimer) m_renewTimer->stop();
    if (m_expiryTimer) m_expiryTimer->stop();
    cancelPending();
}

void RecordingLeaseCoordinator::initialize(RecordingLeasePort* port,
                                           NowProvider nowProvider) {
    m_port = port;
    m_portAvailable = port != nullptr;
    m_nowProvider = std::move(nowProvider);
    if (!m_nowProvider) {
        m_nowProvider = [] { return QDateTime::currentDateTimeUtc(); };
    }

    m_renewTimer = new QTimer(this);
    m_renewTimer->setSingleShot(true);
    m_expiryTimer = new QTimer(this);
    m_expiryTimer->setSingleShot(true);
    connect(m_renewTimer, &QTimer::timeout, this, [this] {
        const QDateTime now = nowUtc();
        if (m_state == RecordingLeaseState::Held &&
            m_nextRenewalAt.isValid() && now < m_nextRenewalAt) {
            qint64 delay = now.msecsTo(m_nextRenewalAt);
            delay = std::clamp<qint64>(delay, 1,
                                       kMaximumTimerIntervalMs);
            m_renewTimer->start(int(delay));
            return;
        }
        renewNow();
    });
    connect(m_expiryTimer, &QTimer::timeout, this,
            [this] { checkExpiry(); });

    qRegisterMetaType<RecordingLeaseState>();
    qRegisterMetaType<CloudProjectTrackLease>();

    if (!port) return;
    connect(port, &RecordingLeasePort::leaseReceived, this,
            [this](quint64 requestId, CloudRequestKind kind,
                   const CloudProjectTrackLease& lease) {
                if (m_dispatchDepth > 0) {
                    DeferredEvent event;
                    event.type = DeferredEvent::Type::Lease;
                    event.requestId = requestId;
                    event.kind = kind;
                    event.lease = lease;
                    m_deferredEvents.push_back(std::move(event));
                    return;
                }
                handleLeaseReceived(requestId, kind, lease);
            });
    connect(port, &RecordingLeasePort::operationCompleted, this,
            [this](quint64 requestId, CloudRequestKind kind,
                   const QString& resourceId) {
                if (m_dispatchDepth > 0) {
                    DeferredEvent event;
                    event.type = DeferredEvent::Type::Completed;
                    event.requestId = requestId;
                    event.kind = kind;
                    event.resourceId = resourceId;
                    m_deferredEvents.push_back(std::move(event));
                    return;
                }
                handleOperationCompleted(requestId, kind, resourceId);
            });
    connect(port, &RecordingLeasePort::requestFailed, this,
            [this](quint64 requestId, CloudRequestKind kind,
                   const CloudClientError& error) {
                if (m_dispatchDepth > 0) {
                    DeferredEvent event;
                    event.type = DeferredEvent::Type::Failed;
                    event.requestId = requestId;
                    event.kind = kind;
                    event.error = error;
                    m_deferredEvents.push_back(std::move(event));
                    return;
                }
                handleRequestFailed(requestId, kind, error);
            });
    connect(port, &RecordingLeasePort::unavailable, this,
            [this] { handlePortUnavailable(); });
    connect(port, &QObject::destroyed, this,
            [this] { handlePortUnavailable(); });
}

QVector<CloudProjectTrackLease> RecordingLeaseCoordinator::leases() const {
    QVector<CloudProjectTrackLease> result;
    QSet<QString> added;
    result.reserve(m_leasesByTrack.size());
    for (const QString& trackId : m_requestedTrackIds) {
        const auto iterator = m_leasesByTrack.constFind(trackId);
        if (iterator == m_leasesByTrack.constEnd()) continue;
        result.push_back(*iterator);
        added.insert(trackId);
    }
    QStringList remaining = m_leasesByTrack.keys();
    std::sort(remaining.begin(), remaining.end());
    for (const QString& trackId : remaining) {
        if (!added.contains(trackId)) result.push_back(m_leasesByTrack[trackId]);
    }
    return result;
}

bool RecordingLeaseCoordinator::acquire(
    const QString& projectId, const QString& sessionId,
    CloudProjectRole role, CloudSessionStatus sessionStatus,
    const QStringList& trackIds, int ttlSeconds) {
    const bool editor = role == CloudProjectRole::Owner ||
                        role == CloudProjectRole::Editor;
    bool tracksValid = trackIds.size() >= 1 &&
                       trackIds.size() <= kMaximumRecordingTracks;
    QSet<QString> uniqueTracks;
    for (const QString& trackId : trackIds) {
        if (!isCanonicalUuid(trackId) || uniqueTracks.contains(trackId)) {
            tracksValid = false;
            break;
        }
        uniqueTracks.insert(trackId);
    }
    if (m_state != RecordingLeaseState::Idle || !m_port ||
        !m_portAvailable || !isCanonicalUuid(projectId) ||
        !isCanonicalUuid(sessionId) || !editor ||
        sessionStatus != CloudSessionStatus::Active || !tracksValid ||
        ttlSeconds < kMinimumLeaseTtlSeconds ||
        ttlSeconds > kMaximumLeaseTtlSeconds) {
        emit acquisitionFailed(localError(
            CloudClientErrorCode::InvalidInput,
            m_state == RecordingLeaseState::Lost
                ? QStringLiteral("Recording lease ownership is uncertain")
                : QStringLiteral("Recording lease request is not allowed")));
        return false;
    }

    ++m_generation;
    m_projectId = projectId;
    m_sessionId = sessionId;
    m_ttlSeconds = ttlSeconds;
    m_requestedTrackIds = trackIds;
    m_leasesByTrack.clear();
    m_acquireDispatchComplete = false;
    m_nextRenewalAt = {};
    setState(RecordingLeaseState::Acquiring);

    const quint64 generation = m_generation;
    for (const QString& trackId : trackIds) {
        if (m_state != RecordingLeaseState::Acquiring ||
            generation != m_generation) {
            break;
        }
        RequestContext context;
        context.operation = RequestOperation::Acquire;
        context.generation = generation;
        context.projectId = projectId;
        context.sessionId = sessionId;
        context.trackId = trackId;
        QPointer<RecordingLeasePort> port = m_port;
        const quint64 requestId = dispatch(
            context, [port, projectId, sessionId, trackId, ttlSeconds] {
                return port ? port->acquireRecordingLease(
                                  projectId, sessionId, trackId, ttlSeconds)
                            : 0;
            });
        if (requestId == 0 && m_state == RecordingLeaseState::Acquiring) {
            failAcquire(localError(
                CloudClientErrorCode::NetworkFailure,
                QStringLiteral("Recording lease request could not start"),
                true));
            break;
        }
    }
    if (m_state == RecordingLeaseState::Acquiring &&
        generation == m_generation) {
        m_acquireDispatchComplete = true;
        finishAcquireIfReady();
    }
    return m_state == RecordingLeaseState::Acquiring ||
           m_state == RecordingLeaseState::Held;
}

void RecordingLeaseCoordinator::releaseAll() {
    if (m_state == RecordingLeaseState::Idle ||
        m_state == RecordingLeaseState::Releasing) {
        return;
    }
    beginRelease();
}

void RecordingLeaseCoordinator::handleSessionChanged(
    const QString& projectId, const QString& sessionId) {
    if (m_state == RecordingLeaseState::Idle) return;
    if (projectId == m_projectId && sessionId == m_sessionId) return;
    beginRelease();
}

void RecordingLeaseCoordinator::handleLogout() {
    if (m_state != RecordingLeaseState::Idle) beginRelease();
}

void RecordingLeaseCoordinator::resetAfterLoss() {
    if (m_state == RecordingLeaseState::Lost) beginRelease(true);
}

quint64 RecordingLeaseCoordinator::dispatch(
    const RequestContext& context,
    const std::function<quint64()>& request) {
    if (!m_port || !m_portAvailable || !request) return 0;
    ++m_dispatchDepth;
    const quint64 requestId = request();
    --m_dispatchDepth;
    if (requestId != 0 && !m_requests.contains(requestId)) {
        m_requests.insert(requestId, context);
    } else if (requestId != 0) {
        if (m_dispatchDepth == 0) m_deferredEvents.clear();
        transitionLost(localError(
            CloudClientErrorCode::InvalidResponse,
            QStringLiteral("Duplicate recording lease request identity")));
        return 0;
    }
    if (m_dispatchDepth == 0) drainDeferredEvents();
    return requestId;
}

void RecordingLeaseCoordinator::drainDeferredEvents() {
    while (m_dispatchDepth == 0 && !m_deferredEvents.isEmpty()) {
        DeferredEvent event = std::move(m_deferredEvents.front());
        m_deferredEvents.removeFirst();
        switch (event.type) {
            case DeferredEvent::Type::Lease:
                handleLeaseReceived(event.requestId, event.kind, event.lease);
                break;
            case DeferredEvent::Type::Completed:
                handleOperationCompleted(event.requestId, event.kind,
                                         event.resourceId);
                break;
            case DeferredEvent::Type::Failed:
                handleRequestFailed(event.requestId, event.kind, event.error);
                break;
        }
    }
}

void RecordingLeaseCoordinator::handleLeaseReceived(
    quint64 requestId, CloudRequestKind kind,
    const CloudProjectTrackLease& lease) {
    const auto iterator = m_requests.find(requestId);
    if (iterator == m_requests.end()) return;
    const RequestContext context = *iterator;
    if (!requestKindMatches(context.operation, kind) ||
        context.generation != m_generation) {
        return;
    }
    const bool commonMatch = lease.kind == CloudProjectLeaseKind::Record &&
                             lease.projectId == context.projectId &&
                             lease.sessionId == context.sessionId &&
                             isCanonicalUuid(lease.id) &&
                             isCanonicalUuid(lease.trackId) &&
                             isCanonicalUuid(lease.holderMemberId) &&
                             leaseTimesValid(lease);
    if (!commonMatch) return;

    if (context.operation == RequestOperation::Acquire) {
        if (m_state != RecordingLeaseState::Acquiring ||
            lease.trackId != context.trackId) {
            return;
        }
        for (auto item = m_leasesByTrack.constBegin();
             item != m_leasesByTrack.constEnd(); ++item) {
            if (item->id == lease.id && item.key() != lease.trackId) {
                m_requests.erase(iterator);
                failAcquire(localError(
                    CloudClientErrorCode::InvalidResponse,
                    QStringLiteral("Duplicate recording lease identity")));
                return;
            }
        }
        m_requests.erase(iterator);
        m_leasesByTrack.insert(lease.trackId, lease);
        finishAcquireIfReady();
        return;
    }

    if (context.operation == RequestOperation::Renew) {
        if (m_state != RecordingLeaseState::Held ||
            lease.id != context.leaseId || lease.trackId != context.trackId) {
            return;
        }
        m_requests.erase(iterator);
        m_leasesByTrack.insert(context.trackId, lease);
        if (lease.expiresAt <= nowUtc()) {
            transitionLost(localError(
                CloudClientErrorCode::Timeout,
                QStringLiteral("Recording lease expired"), true));
            return;
        }
        finishRenewIfReady();
    }
}

void RecordingLeaseCoordinator::handleOperationCompleted(
    quint64 requestId, CloudRequestKind kind, const QString& resourceId) {
    const auto iterator = m_requests.find(requestId);
    if (iterator == m_requests.end()) return;
    const RequestContext context = *iterator;
    if (context.operation != RequestOperation::Release ||
        !requestKindMatches(context.operation, kind) ||
        context.generation != m_generation ||
        resourceId != context.leaseId) {
        return;
    }
    m_requests.erase(iterator);
    const auto lease = m_leasesByTrack.constFind(context.trackId);
    if (lease != m_leasesByTrack.constEnd() &&
        lease->id == context.leaseId) {
        m_leasesByTrack.remove(context.trackId);
    }
    finishReleaseIfReady();
}

void RecordingLeaseCoordinator::handleRequestFailed(
    quint64 requestId, CloudRequestKind kind,
    const CloudClientError& error) {
    const auto iterator = m_requests.find(requestId);
    if (iterator == m_requests.end()) return;
    const RequestContext context = *iterator;
    if (!requestKindMatches(context.operation, kind) ||
        context.generation != m_generation) {
        return;
    }
    m_requests.erase(iterator);
    switch (context.operation) {
        case RequestOperation::Acquire:
            if (m_state == RecordingLeaseState::Acquiring)
                failAcquire(error);
            break;
        case RequestOperation::Renew:
            if (m_state == RecordingLeaseState::Held)
                transitionLost(error);
            break;
        case RequestOperation::Release: {
            const auto lease = m_leasesByTrack.constFind(context.trackId);
            if (lease != m_leasesByTrack.constEnd() &&
                lease->id == context.leaseId) {
                m_leasesByTrack.remove(context.trackId);
            }
            finishReleaseIfReady();
            break;
        }
    }
}

void RecordingLeaseCoordinator::handlePortUnavailable() {
    if (!m_portAvailable) return;
    m_portAvailable = false;
    if (m_renewTimer) m_renewTimer->stop();
    if (m_expiryTimer) m_expiryTimer->stop();
    ++m_generation;
    m_requests.clear();
    m_deferredEvents.clear();
    m_nextRenewalAt = {};

    if (m_state == RecordingLeaseState::Idle) return;
    if (m_state == RecordingLeaseState::Releasing) {
        m_leasesByTrack.clear();
        clearContext();
        setState(RecordingLeaseState::Idle);
        emit releaseFinished();
        return;
    }
    transitionLost(localError(
        CloudClientErrorCode::NetworkFailure,
        QStringLiteral("Recording lease client is unavailable"), true));
}

void RecordingLeaseCoordinator::finishAcquireIfReady() {
    if (m_state != RecordingLeaseState::Acquiring ||
        !m_acquireDispatchComplete) {
        return;
    }
    for (auto iterator = m_requests.constBegin();
         iterator != m_requests.constEnd(); ++iterator) {
        if (iterator->generation == m_generation &&
            iterator->operation == RequestOperation::Acquire) {
            return;
        }
    }
    if (m_leasesByTrack.size() != m_requestedTrackIds.size()) {
        failAcquire(localError(
            CloudClientErrorCode::InvalidResponse,
            QStringLiteral("Recording lease set is incomplete")));
        return;
    }
    const QDateTime now = nowUtc();
    for (const CloudProjectTrackLease& lease : std::as_const(m_leasesByTrack)) {
        if (!lease.expiresAt.isValid() || lease.expiresAt <= now) {
            // A syntactically valid response can already be stale because of a
            // delayed callback or clock skew. It is an acquisition failure, not
            // a Held set: in particular, never tell the recording UI to start.
            failAcquire(localError(
                CloudClientErrorCode::Timeout,
                QStringLiteral("Recording lease expired before acquisition"),
                true));
            return;
        }
    }
    setState(RecordingLeaseState::Held);
    scheduleHeartbeat();
    // scheduleHeartbeat performs a second expiry check with a fresh clock read.
    // Preserve the no-stale-start guarantee even if the clock advanced across
    // the boundary above.
    if (m_state == RecordingLeaseState::Held)
        emit leasesAcquired(leases());
}

void RecordingLeaseCoordinator::failAcquire(const CloudClientError& error) {
    if (m_state != RecordingLeaseState::Acquiring) return;
    emit acquisitionFailed(error);
    ++m_generation;
    cancelPending();
    if (m_leasesByTrack.isEmpty()) {
        clearContext();
        setState(RecordingLeaseState::Idle);
        return;
    }
    beginRelease();
}

void RecordingLeaseCoordinator::beginRelease(bool fromReset) {
    Q_UNUSED(fromReset);
    if (m_state == RecordingLeaseState::Releasing) return;
    if (m_renewTimer) m_renewTimer->stop();
    if (m_expiryTimer) m_expiryTimer->stop();
    m_nextRenewalAt = {};
    ++m_generation;
    cancelPending();
    setState(RecordingLeaseState::Releasing);
    m_releaseDispatchComplete = false;

    const QVector<CloudProjectTrackLease> knownLeases = leases();
    if (!m_port || !m_portAvailable) {
        m_leasesByTrack.clear();
    } else {
        const quint64 generation = m_generation;
        for (const CloudProjectTrackLease& lease : knownLeases) {
            if (m_state != RecordingLeaseState::Releasing ||
                generation != m_generation) {
                break;
            }
            RequestContext context;
            context.operation = RequestOperation::Release;
            context.generation = generation;
            context.projectId = m_projectId;
            context.sessionId = m_sessionId;
            context.trackId = lease.trackId;
            context.leaseId = lease.id;
            QPointer<RecordingLeasePort> port = m_port;
            const QString projectId = m_projectId;
            const QString sessionId = m_sessionId;
            const QString leaseId = lease.id;
            const quint64 requestId = dispatch(
                context, [port, projectId, sessionId, leaseId] {
                    return port ? port->releaseRecordingLease(
                                      projectId, sessionId, leaseId)
                                : 0;
                });
            if (requestId == 0) m_leasesByTrack.remove(lease.trackId);
        }
    }
    if (m_state == RecordingLeaseState::Releasing) {
        m_releaseDispatchComplete = true;
        finishReleaseIfReady();
    }
}

void RecordingLeaseCoordinator::finishReleaseIfReady() {
    if (m_state != RecordingLeaseState::Releasing ||
        !m_releaseDispatchComplete) {
        return;
    }
    for (auto iterator = m_requests.constBegin();
         iterator != m_requests.constEnd(); ++iterator) {
        if (iterator->generation == m_generation &&
            iterator->operation == RequestOperation::Release) {
            return;
        }
    }
    m_leasesByTrack.clear();
    clearContext();
    setState(RecordingLeaseState::Idle);
    emit releaseFinished();
}

void RecordingLeaseCoordinator::renewNow() {
    if (m_state != RecordingLeaseState::Held || !m_port ||
        !m_portAvailable) {
        return;
    }
    checkExpiry();
    if (m_state != RecordingLeaseState::Held) return;
    for (auto iterator = m_requests.constBegin();
         iterator != m_requests.constEnd(); ++iterator) {
        if (iterator->generation == m_generation &&
            iterator->operation == RequestOperation::Renew) {
            return;
        }
    }

    m_renewTimer->stop();
    m_nextRenewalAt = {};
    m_renewDispatchComplete = false;
    const quint64 generation = m_generation;
    const QVector<CloudProjectTrackLease> current = leases();
    for (const CloudProjectTrackLease& lease : current) {
        if (m_state != RecordingLeaseState::Held ||
            generation != m_generation) {
            break;
        }
        RequestContext context;
        context.operation = RequestOperation::Renew;
        context.generation = generation;
        context.projectId = m_projectId;
        context.sessionId = m_sessionId;
        context.trackId = lease.trackId;
        context.leaseId = lease.id;
        QPointer<RecordingLeasePort> port = m_port;
        const QString projectId = m_projectId;
        const QString sessionId = m_sessionId;
        const QString leaseId = lease.id;
        const int ttlSeconds = m_ttlSeconds;
        const quint64 requestId = dispatch(
            context, [port, projectId, sessionId, leaseId, ttlSeconds] {
                return port ? port->renewRecordingLease(
                                  projectId, sessionId, leaseId, ttlSeconds)
                            : 0;
            });
        if (requestId == 0 && m_state == RecordingLeaseState::Held) {
            transitionLost(localError(
                CloudClientErrorCode::NetworkFailure,
                QStringLiteral("Recording lease renewal could not start"),
                true));
            break;
        }
    }
    if (m_state == RecordingLeaseState::Held &&
        generation == m_generation) {
        m_renewDispatchComplete = true;
        finishRenewIfReady();
    }
}

void RecordingLeaseCoordinator::finishRenewIfReady() {
    if (m_state != RecordingLeaseState::Held ||
        !m_renewDispatchComplete) {
        return;
    }
    for (auto iterator = m_requests.constBegin();
         iterator != m_requests.constEnd(); ++iterator) {
        if (iterator->generation == m_generation &&
            iterator->operation == RequestOperation::Renew) {
            return;
        }
    }
    scheduleHeartbeat();
}

void RecordingLeaseCoordinator::scheduleHeartbeat() {
    if (m_state != RecordingLeaseState::Held || m_leasesByTrack.isEmpty())
        return;
    const QDateTime now = nowUtc();
    QDateTime earliest;
    for (const CloudProjectTrackLease& lease : std::as_const(m_leasesByTrack)) {
        if (!earliest.isValid() || lease.expiresAt < earliest)
            earliest = lease.expiresAt;
    }
    if (!earliest.isValid() || earliest <= now) {
        transitionLost(localError(CloudClientErrorCode::Timeout,
                                  QStringLiteral("Recording lease expired"),
                                  true));
        return;
    }
    const qint64 leadMs = std::clamp<qint64>(
        qint64(m_ttlSeconds) * 1000 / 3, 1000, 30'000);
    m_nextRenewalAt = earliest.addMSecs(-leadMs);
    qint64 delay = now.msecsTo(m_nextRenewalAt);
    if (delay < 1) delay = 1;
    delay = std::min(delay, kMaximumTimerIntervalMs);
    m_renewTimer->start(int(delay));
    scheduleExpiryCheck();
}

void RecordingLeaseCoordinator::scheduleExpiryCheck() {
    if (m_state != RecordingLeaseState::Held || m_leasesByTrack.isEmpty())
        return;
    const QDateTime now = nowUtc();
    QDateTime earliest;
    for (const CloudProjectTrackLease& lease : std::as_const(m_leasesByTrack)) {
        if (!earliest.isValid() || lease.expiresAt < earliest)
            earliest = lease.expiresAt;
    }
    qint64 delay = earliest.isValid() ? now.msecsTo(earliest) : 0;
    if (delay <= 0) {
        transitionLost(localError(CloudClientErrorCode::Timeout,
                                  QStringLiteral("Recording lease expired"),
                                  true));
        return;
    }
    delay = std::min(delay, kMaximumTimerIntervalMs);
    m_expiryTimer->start(int(std::max<qint64>(1, delay)));
}

void RecordingLeaseCoordinator::checkExpiry() {
    if (m_state != RecordingLeaseState::Held) return;
    const QDateTime now = nowUtc();
    for (const CloudProjectTrackLease& lease : std::as_const(m_leasesByTrack)) {
        if (!lease.expiresAt.isValid() || lease.expiresAt <= now) {
            transitionLost(localError(
                CloudClientErrorCode::Timeout,
                QStringLiteral("Recording lease expired"), true));
            return;
        }
    }
    scheduleExpiryCheck();
}

void RecordingLeaseCoordinator::transitionLost(
    const CloudClientError& error) {
    if (m_state == RecordingLeaseState::Lost) return;
    if (m_renewTimer) m_renewTimer->stop();
    if (m_expiryTimer) m_expiryTimer->stop();
    m_nextRenewalAt = {};
    ++m_generation;
    cancelPending();
    setState(RecordingLeaseState::Lost);
    emit leaseLost(error, leases());
}

void RecordingLeaseCoordinator::cancelPending() {
    const QList<quint64> requestIds = m_requests.keys();
    m_requests.clear();
    QPointer<RecordingLeasePort> port = m_port;
    if (!port || !m_portAvailable) return;
    for (quint64 requestId : requestIds) port->cancel(requestId);
}

void RecordingLeaseCoordinator::clearContext() {
    m_projectId.clear();
    m_sessionId.clear();
    m_requestedTrackIds.clear();
    m_nextRenewalAt = {};
    m_acquireDispatchComplete = false;
    m_renewDispatchComplete = false;
    m_releaseDispatchComplete = false;
}

void RecordingLeaseCoordinator::setState(RecordingLeaseState state) {
    if (m_state == state) return;
    m_state = state;
    emit stateChanged(state);
}

QDateTime RecordingLeaseCoordinator::nowUtc() const {
    QDateTime value = m_nowProvider ? m_nowProvider()
                                    : QDateTime::currentDateTimeUtc();
    return value.toUTC();
}

bool RecordingLeaseCoordinator::isCanonicalUuid(const QString& value) {
    const QUuid uuid(value);
    return !uuid.isNull() &&
           value == uuid.toString(QUuid::WithoutBraces).toLower();
}

bool RecordingLeaseCoordinator::requestKindMatches(
    RequestOperation operation, CloudRequestKind kind) {
    switch (operation) {
        case RequestOperation::Acquire:
            return kind == CloudRequestKind::AcquireRecordingLease;
        case RequestOperation::Renew:
            return kind == CloudRequestKind::RenewRecordingLease;
        case RequestOperation::Release:
            return kind == CloudRequestKind::ReleaseRecordingLease;
    }
    return false;
}

CloudClientError RecordingLeaseCoordinator::localError(
    CloudClientErrorCode code, const QString& message, bool retryable) {
    CloudClientError error;
    error.code = code;
    error.safeMessage = message.left(240);
    error.retryable = retryable;
    return error;
}

namespace {

class FakeRecordingLeasePort final : public RecordingLeasePort {
public:
    enum class CallKind : quint8 { Acquire, Renew, Release };
    struct Call {
        CallKind kind = CallKind::Acquire;
        quint64 requestId = 0;
        QString projectId;
        QString sessionId;
        QString resourceId;
        int ttlSeconds = 0;
    };

    using RecordingLeasePort::RecordingLeasePort;

    quint64 acquireRecordingLease(const QString& projectId,
                                   const QString& sessionId,
                                   const QString& trackId,
                                   int ttlSeconds) override {
        const quint64 requestId = add(CallKind::Acquire, projectId, sessionId,
                                      trackId, ttlSeconds);
        if (synchronousAcquire) {
            synchronousAcquire = false;
            emit leaseReceived(requestId,
                               CloudRequestKind::AcquireRecordingLease,
                               synchronousLease);
        }
        return requestId;
    }

    quint64 renewRecordingLease(const QString& projectId,
                                 const QString& sessionId,
                                 const QString& leaseId,
                                 int ttlSeconds) override {
        return add(CallKind::Renew, projectId, sessionId, leaseId,
                   ttlSeconds);
    }

    quint64 releaseRecordingLease(const QString& projectId,
                                   const QString& sessionId,
                                   const QString& leaseId) override {
        return add(CallKind::Release, projectId, sessionId, leaseId, 0);
    }

    bool cancel(quint64 requestId) override {
        cancelled.push_back(requestId);
        return true;
    }

    void deliverLease(quint64 requestId, CloudRequestKind kind,
                      const CloudProjectTrackLease& lease) {
        emit leaseReceived(requestId, kind, lease);
    }

    void complete(quint64 requestId, const QString& leaseId) {
        emit operationCompleted(requestId,
                                CloudRequestKind::ReleaseRecordingLease,
                                leaseId);
    }

    void fail(quint64 requestId, CloudRequestKind kind,
              const CloudClientError& error) {
        emit requestFailed(requestId, kind, error);
    }

    QVector<Call> calls;
    QVector<quint64> cancelled;
    QVector<quint64> forcedRequestIds;
    bool synchronousAcquire = false;
    CloudProjectTrackLease synchronousLease;

private:
    quint64 add(CallKind kind, const QString& projectId,
                const QString& sessionId, const QString& resourceId,
                int ttlSeconds) {
        quint64 requestId = m_nextRequestId++;
        if (!forcedRequestIds.isEmpty()) {
            requestId = forcedRequestIds.front();
            forcedRequestIds.removeFirst();
        }
        calls.push_back(
            {kind, requestId, projectId, sessionId, resourceId, ttlSeconds});
        return requestId;
    }

    quint64 m_nextRequestId = 1;
};

CloudProjectTrackLease testLease(const QString& projectId,
                                 const QString& sessionId,
                                 const QString& trackId,
                                 const QString& leaseId,
                                 const QDateTime& acquiredAt,
                                 const QDateTime& expiresAt) {
    CloudProjectTrackLease lease;
    lease.id = leaseId;
    lease.projectId = projectId;
    lease.sessionId = sessionId;
    lease.trackId = trackId;
    lease.kind = CloudProjectLeaseKind::Record;
    lease.holderMemberId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    lease.acquiredAt = acquiredAt;
    lease.renewedAt = acquiredAt;
    lease.expiresAt = expiresAt;
    return lease;
}

CloudClientError testFailure(const QString& message) {
    CloudClientError error;
    error.code = CloudClientErrorCode::NetworkFailure;
    error.safeMessage = message;
    error.retryable = true;
    return error;
}

} // namespace

bool checkRecordingLeaseCoordinatorForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString projectId =
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const QString otherProjectId =
        QStringLiteral("99999999-9999-4999-8999-999999999999");
    const QString sessionId =
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const QString otherSessionId =
        QStringLiteral("88888888-8888-4888-8888-888888888888");
    const QString trackA =
        QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    const QString trackB =
        QStringLiteral("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    const QString trackC =
        QStringLiteral("77777777-7777-4777-8777-777777777777");
    const QString leaseA =
        QStringLiteral("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    const QString leaseB =
        QStringLiteral("ffffffff-ffff-4fff-8fff-ffffffffffff");
    const QString leaseC =
        QStringLiteral("66666666-6666-4666-8666-666666666666");
    QDateTime now =
        QDateTime::fromString(QStringLiteral("2026-08-30T12:00:00Z"),
                              Qt::ISODate);

    FakeRecordingLeasePort port;
    RecordingLeaseCoordinator coordinator(&port, [&now] { return now; });
    int acquisitionFailures = 0;
    int acquiredSignals = 0;
    int lostSignals = 0;
    int releaseSignals = 0;
    QVector<CloudProjectTrackLease> recovery;
    QObject::connect(
        &coordinator, &RecordingLeaseCoordinator::acquisitionFailed,
        [&](const CloudClientError&) { ++acquisitionFailures; });
    QObject::connect(
        &coordinator, &RecordingLeaseCoordinator::leasesAcquired,
        [&](const QVector<CloudProjectTrackLease>&) { ++acquiredSignals; });
    QObject::connect(
        &coordinator, &RecordingLeaseCoordinator::leaseLost,
        [&](const CloudClientError&,
            const QVector<CloudProjectTrackLease>& leases) {
            ++lostSignals;
            recovery = leases;
        });
    QObject::connect(&coordinator,
                     &RecordingLeaseCoordinator::releaseFinished,
                     [&] { ++releaseSignals; });

    QStringList tooManyTracks;
    for (int index = 1; index <= 9; ++index) {
        tooManyTracks.push_back(
            QStringLiteral("00000000-0000-4000-8000-%1")
                .arg(index, 12, 16, QLatin1Char('0')));
    }

    if (coordinator.acquire(projectId, sessionId, CloudProjectRole::Viewer,
                            CloudSessionStatus::Active, {trackA}, 30) ||
        coordinator.acquire(projectId.toUpper(), sessionId,
                            CloudProjectRole::Editor,
                            CloudSessionStatus::Active, {trackA}, 30) ||
        coordinator.acquire(projectId, sessionId, CloudProjectRole::Editor,
                            CloudSessionStatus::Active,
                            {trackA, trackA}, 30) ||
        coordinator.acquire(projectId, sessionId, CloudProjectRole::Editor,
                            CloudSessionStatus::Ending, {trackA}, 30) ||
        coordinator.acquire(projectId, sessionId, CloudProjectRole::Editor,
                            CloudSessionStatus::Active, tooManyTracks, 30) ||
        coordinator.acquire(projectId, sessionId, CloudProjectRole::Editor,
                            CloudSessionStatus::Active, {trackA}, 4) ||
        acquisitionFailures != 6 || !port.calls.isEmpty() ||
        coordinator.state() != RecordingLeaseState::Idle) {
        return fail(QStringLiteral("invalid recording context was dispatched"));
    }

    // A response can be structurally valid yet already expired after network
    // delay or clock skew. It must roll back like a failed acquisition and, most
    // importantly, must never emit the signal that starts audio capture.
    {
        FakeRecordingLeasePort expiredPort;
        RecordingLeaseCoordinator expired(&expiredPort,
                                           [&now] { return now; });
        int expiredAcquired = 0;
        int expiredFailed = 0;
        int expiredHeldStates = 0;
        QObject::connect(
            &expired, &RecordingLeaseCoordinator::leasesAcquired,
            [&](const QVector<CloudProjectTrackLease>&) { ++expiredAcquired; });
        QObject::connect(
            &expired, &RecordingLeaseCoordinator::acquisitionFailed,
            [&](const CloudClientError&) { ++expiredFailed; });
        QObject::connect(
            &expired, &RecordingLeaseCoordinator::stateChanged,
            [&](RecordingLeaseState state) {
                if (state == RecordingLeaseState::Held) ++expiredHeldStates;
            });
        if (!expired.acquire(projectId, sessionId, CloudProjectRole::Editor,
                             CloudSessionStatus::Active, {trackA}, 30) ||
            expiredPort.calls.size() != 1) {
            return fail(QStringLiteral("expired-lease fixture did not dispatch"));
        }
        const quint64 requestId = expiredPort.calls.front().requestId;
        expiredPort.deliverLease(
            requestId, CloudRequestKind::AcquireRecordingLease,
            testLease(projectId, sessionId, trackA, leaseA,
                      now.addSecs(-30), now.addSecs(-1)));
        if (expiredAcquired != 0 || expiredHeldStates != 0 ||
            expiredFailed != 1 ||
            expired.state() != RecordingLeaseState::Releasing ||
            expiredPort.calls.size() != 2 ||
            expiredPort.calls.back().kind !=
                FakeRecordingLeasePort::CallKind::Release) {
            return fail(QStringLiteral(
                "already-expired lease was exposed as acquired"));
        }
        const auto release = expiredPort.calls.back();
        expiredPort.complete(release.requestId, release.resourceId);
        if (expired.state() != RecordingLeaseState::Idle) {
            return fail(QStringLiteral(
                "already-expired lease rollback did not finish"));
        }
    }

    acquisitionFailures = 0;
    if (!coordinator.acquire(projectId, sessionId, CloudProjectRole::Editor,
                             CloudSessionStatus::Active,
                             {trackA, trackB, trackC}, 30) ||
        coordinator.state() != RecordingLeaseState::Acquiring ||
        port.calls.size() != 3 ||
        port.calls[0].kind != FakeRecordingLeasePort::CallKind::Acquire ||
        port.calls[1].kind != FakeRecordingLeasePort::CallKind::Acquire ||
        port.calls[2].kind != FakeRecordingLeasePort::CallKind::Acquire) {
        return fail(QStringLiteral("recording leases were not acquired in parallel"));
    }
    const quint64 acquireA = port.calls[0].requestId;
    const quint64 acquireB = port.calls[1].requestId;
    const quint64 acquireC = port.calls[2].requestId;
    const CloudProjectTrackLease grantedA = testLease(
        projectId, sessionId, trackA, leaseA, now, now.addSecs(30));
    CloudProjectTrackLease crossProject = grantedA;
    crossProject.projectId = otherProjectId;
    port.deliverLease(acquireA, CloudRequestKind::AcquireRecordingLease,
                      crossProject);
    if (!coordinator.leases().isEmpty() ||
        coordinator.state() != RecordingLeaseState::Acquiring) {
        return fail(QStringLiteral("cross-project lease callback was applied"));
    }
    port.deliverLease(acquireA, CloudRequestKind::AcquireRecordingLease,
                      grantedA);
    port.fail(acquireB, CloudRequestKind::AcquireRecordingLease,
              testFailure(QStringLiteral("lease denied")));
    if (coordinator.state() != RecordingLeaseState::Releasing ||
        coordinator.leases().size() != 1 || acquisitionFailures != 1 ||
        !port.cancelled.contains(acquireC) || port.calls.size() != 4 ||
        port.calls.back().kind != FakeRecordingLeasePort::CallKind::Release ||
        port.calls.back().resourceId != leaseA) {
        return fail(QStringLiteral("partial lease set did not roll back"));
    }
    const quint64 rollbackRelease = port.calls.back().requestId;
    port.deliverLease(
        acquireC, CloudRequestKind::AcquireRecordingLease,
        testLease(projectId, sessionId, trackC, leaseC, now,
                  now.addSecs(30)));
    if (coordinator.leases().size() != 1 ||
        coordinator.state() != RecordingLeaseState::Releasing) {
        return fail(QStringLiteral("stale acquire callback changed rollback"));
    }
    port.complete(rollbackRelease, leaseA);
    if (coordinator.state() != RecordingLeaseState::Idle ||
        !coordinator.leases().isEmpty() || releaseSignals != 1) {
        return fail(QStringLiteral("partial lease rollback did not finish"));
    }

    port.calls.clear();
    port.cancelled.clear();
    if (!coordinator.acquire(projectId, sessionId, CloudProjectRole::Owner,
                             CloudSessionStatus::Active,
                             {trackA, trackB}, 30) ||
        port.calls.size() != 2) {
        return fail(QStringLiteral("valid recording lease set was rejected"));
    }
    const quint64 heldAcquireA = port.calls[0].requestId;
    const quint64 heldAcquireB = port.calls[1].requestId;
    port.deliverLease(
        heldAcquireA, CloudRequestKind::AcquireRecordingLease,
        testLease(projectId, sessionId, trackA, leaseA, now,
                  now.addSecs(30)));
    port.deliverLease(
        heldAcquireB, CloudRequestKind::AcquireRecordingLease,
        testLease(projectId, sessionId, trackB, leaseB, now,
                  now.addSecs(35)));
    if (coordinator.state() != RecordingLeaseState::Held ||
        coordinator.leases().size() != 2 || acquiredSignals != 1 ||
        !coordinator.nextRenewalAt().isValid() ||
        coordinator.nextRenewalAt() <= now ||
        coordinator.nextRenewalAt() >= now.addSecs(30)) {
        return fail(QStringLiteral("complete lease set did not enter Held"));
    }

    const qsizetype callsBeforeRenew = port.calls.size();
    coordinator.renewNow();
    if (port.calls.size() != callsBeforeRenew + 2 ||
        port.calls[callsBeforeRenew].kind !=
            FakeRecordingLeasePort::CallKind::Renew ||
        port.calls[callsBeforeRenew + 1].kind !=
            FakeRecordingLeasePort::CallKind::Renew ||
        port.calls[callsBeforeRenew].ttlSeconds != 30 ||
        port.calls[callsBeforeRenew + 1].ttlSeconds != 30) {
        return fail(QStringLiteral("lease heartbeat was not dispatched"));
    }
    const quint64 renewA = port.calls[callsBeforeRenew].requestId;
    const quint64 renewB = port.calls[callsBeforeRenew + 1].requestId;
    CloudProjectTrackLease renewedA = testLease(
        projectId, sessionId, trackA, leaseA, now, now.addSecs(60));
    CloudProjectTrackLease renewedB = testLease(
        projectId, sessionId, trackB, leaseB, now, now.addSecs(65));
    CloudProjectTrackLease crossSession = renewedA;
    crossSession.sessionId = otherSessionId;
    port.deliverLease(renewA, CloudRequestKind::RenewRecordingLease,
                      crossSession);
    if (coordinator.leases().front().expiresAt != now.addSecs(30)) {
        return fail(QStringLiteral("cross-session renewal was applied"));
    }
    now = now.addSecs(1);
    renewedA.renewedAt = now;
    renewedB.renewedAt = now;
    port.deliverLease(renewA, CloudRequestKind::RenewRecordingLease, renewedA);
    port.deliverLease(renewB, CloudRequestKind::RenewRecordingLease, renewedB);
    if (coordinator.state() != RecordingLeaseState::Held ||
        coordinator.leases().size() != 2 ||
        !coordinator.nextRenewalAt().isValid()) {
        return fail(QStringLiteral("lease heartbeat responses were not applied"));
    }

    const qsizetype callsBeforeFailedRenew = port.calls.size();
    coordinator.renewNow();
    if (port.calls.size() != callsBeforeFailedRenew + 2) {
        return fail(QStringLiteral("second lease heartbeat was not dispatched"));
    }
    const quint64 failedRenew = port.calls[callsBeforeFailedRenew].requestId;
    const quint64 staleRenew =
        port.calls[callsBeforeFailedRenew + 1].requestId;
    port.fail(failedRenew, CloudRequestKind::RenewRecordingLease,
              testFailure(QStringLiteral("renew failed")));
    if (coordinator.state() != RecordingLeaseState::Lost ||
        lostSignals != 1 || recovery.size() != 2 ||
        !port.cancelled.contains(staleRenew)) {
        return fail(QStringLiteral("renew failure did not preserve recovery"));
    }
    port.deliverLease(staleRenew, CloudRequestKind::RenewRecordingLease,
                      renewedB);
    const qsizetype callsWhileLost = port.calls.size();
    if (coordinator.acquire(projectId, sessionId, CloudProjectRole::Editor,
                            CloudSessionStatus::Active, {trackC}, 30) ||
        port.calls.size() != callsWhileLost ||
        coordinator.leases().size() != 2) {
        return fail(QStringLiteral("Lost state allowed another acquisition"));
    }

    coordinator.resetAfterLoss();
    if (coordinator.state() != RecordingLeaseState::Releasing ||
        port.calls.size() != callsWhileLost + 2) {
        return fail(QStringLiteral("explicit Lost reset did not release leases"));
    }
    const quint64 resetReleaseA = port.calls[callsWhileLost].requestId;
    const quint64 resetReleaseB = port.calls[callsWhileLost + 1].requestId;
    port.complete(resetReleaseA, port.calls[callsWhileLost].resourceId);
    port.fail(resetReleaseB, CloudRequestKind::ReleaseRecordingLease,
              testFailure(QStringLiteral("release already expired")));
    if (coordinator.state() != RecordingLeaseState::Idle ||
        !coordinator.leases().isEmpty() || releaseSignals != 2) {
        return fail(QStringLiteral("Lost reset did not return to Idle"));
    }

    port.calls.clear();
    if (!coordinator.acquire(projectId, sessionId, CloudProjectRole::Editor,
                             CloudSessionStatus::Active, {trackC}, 120)) {
        return fail(QStringLiteral("maximum lease TTL was rejected"));
    }
    const quint64 changedSessionAcquire = port.calls.back().requestId;
    port.deliverLease(
        changedSessionAcquire, CloudRequestKind::AcquireRecordingLease,
        testLease(projectId, sessionId, trackC, leaseC, now,
                  now.addSecs(120)));
    coordinator.handleSessionChanged(projectId, otherSessionId);
    if (coordinator.state() != RecordingLeaseState::Releasing ||
        port.calls.back().kind != FakeRecordingLeasePort::CallKind::Release ||
        port.calls.back().projectId != projectId ||
        port.calls.back().sessionId != sessionId) {
        return fail(QStringLiteral("session change did not release old leases"));
    }
    const auto changedSessionRelease = port.calls.back();
    port.complete(changedSessionRelease.requestId,
                  changedSessionRelease.resourceId);
    if (coordinator.state() != RecordingLeaseState::Idle) {
        return fail(QStringLiteral("session-change release did not finish"));
    }

    port.calls.clear();
    if (!coordinator.acquire(projectId, sessionId, CloudProjectRole::Editor,
                             CloudSessionStatus::Active, {trackA}, 5)) {
        return fail(QStringLiteral("minimum lease TTL was rejected"));
    }
    const quint64 expiringAcquire = port.calls.back().requestId;
    port.deliverLease(
        expiringAcquire, CloudRequestKind::AcquireRecordingLease,
        testLease(projectId, sessionId, trackA, leaseA, now,
                  now.addSecs(5)));
    now = now.addSecs(6);
    coordinator.checkExpiry();
    if (coordinator.state() != RecordingLeaseState::Lost ||
        lostSignals != 2 || coordinator.leases().size() != 1) {
        return fail(QStringLiteral("expired lease did not enter Lost"));
    }
    coordinator.handleLogout();
    if (coordinator.state() != RecordingLeaseState::Releasing) {
        return fail(QStringLiteral("logout did not release recovery lease"));
    }
    const auto logoutRelease = port.calls.back();
    port.complete(logoutRelease.requestId, logoutRelease.resourceId);
    if (coordinator.state() != RecordingLeaseState::Idle) {
        return fail(QStringLiteral("logout release did not finish"));
    }

    {
        FakeRecordingLeasePort synchronousPort;
        RecordingLeaseCoordinator synchronous(
            &synchronousPort, [&now] { return now; });
        synchronousPort.synchronousLease = testLease(
            projectId, sessionId, trackA, leaseA, now, now.addSecs(30));
        synchronousPort.synchronousAcquire = true;
        if (!synchronous.acquire(projectId, sessionId,
                                 CloudProjectRole::Editor,
                                 CloudSessionStatus::Active, {trackA}, 30) ||
            synchronous.state() != RecordingLeaseState::Held ||
            synchronous.leases().size() != 1) {
            return fail(QStringLiteral("synchronous lease callback was lost"));
        }
        synchronous.releaseAll();
        if (synchronous.state() != RecordingLeaseState::Releasing ||
            synchronousPort.calls.back().kind !=
                FakeRecordingLeasePort::CallKind::Release) {
            return fail(QStringLiteral("synchronous fixture did not release"));
        }
        const auto release = synchronousPort.calls.back();
        synchronousPort.complete(release.requestId, release.resourceId);
        if (synchronous.state() != RecordingLeaseState::Idle)
            return fail(QStringLiteral("synchronous fixture stayed active"));
    }

    {
        FakeRecordingLeasePort collisionPort;
        collisionPort.forcedRequestIds = {73, 73};
        RecordingLeaseCoordinator collision(
            &collisionPort, [&now] { return now; });
        int collisionLosses = 0;
        QObject::connect(
            &collision, &RecordingLeaseCoordinator::leaseLost,
            [&](const CloudClientError&,
                const QVector<CloudProjectTrackLease>&) {
                ++collisionLosses;
            });
        if (collision.acquire(projectId, sessionId, CloudProjectRole::Editor,
                              CloudSessionStatus::Active,
                              {trackA, trackB}, 30) ||
            collision.state() != RecordingLeaseState::Lost ||
            collisionLosses != 1 || collisionPort.calls.size() != 2 ||
            !collisionPort.cancelled.contains(73)) {
            return fail(QStringLiteral("request-id collision was not fail-closed"));
        }
        collisionPort.deliverLease(
            73, CloudRequestKind::AcquireRecordingLease,
            testLease(projectId, sessionId, trackA, leaseA, now,
                      now.addSecs(30)));
        if (collision.state() != RecordingLeaseState::Lost ||
            !collision.leases().isEmpty()) {
            return fail(QStringLiteral("colliding stale callback changed state"));
        }
        collision.resetAfterLoss();
        if (collision.state() != RecordingLeaseState::Idle)
            return fail(QStringLiteral("collision reset did not recover"));
    }

    auto* disappearingPort = new FakeRecordingLeasePort;
    RecordingLeaseCoordinator disappearing(
        disappearingPort, [&now] { return now; });
    if (!disappearing.acquire(projectId, sessionId,
                              CloudProjectRole::Editor,
                              CloudSessionStatus::Active, {trackB}, 30)) {
        delete disappearingPort;
        return fail(QStringLiteral("destruction fixture did not acquire"));
    }
    const quint64 disappearingAcquire =
        disappearingPort->calls.back().requestId;
    disappearingPort->deliverLease(
        disappearingAcquire, CloudRequestKind::AcquireRecordingLease,
        testLease(projectId, sessionId, trackB, leaseB, now,
                  now.addSecs(30)));
    delete disappearingPort;
    if (disappearing.state() != RecordingLeaseState::Lost ||
        disappearing.leases().size() != 1) {
        return fail(QStringLiteral("port destruction did not preserve recovery"));
    }
    disappearing.renewNow();
    disappearing.resetAfterLoss();
    if (disappearing.state() != RecordingLeaseState::Idle ||
        !disappearing.leases().isEmpty()) {
        return fail(QStringLiteral("destroyed port was used during reset"));
    }

    return true;
}

} // namespace collab
