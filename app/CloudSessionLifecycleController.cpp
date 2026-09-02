#include "CloudSessionLifecycleController.hpp"

#include "CloudProjectSyncCoordinator.hpp"
#include "CollaborationService.hpp"

#include <QPointer>
#include <QUuid>

#include <functional>
#include <optional>
#include <utility>

namespace collab {
namespace {

QString canonicalUuid(const QString& value) {
    const QUuid uuid(value.trimmed());
    return uuid.isNull()
        ? QString()
        : uuid.toString(QUuid::WithoutBraces).toLower();
}

bool blocksCloudRequests(CollaborationState state) {
    return state == CollaborationState::SignedOut ||
           state == CollaborationState::NoConnection ||
           state == CollaborationState::Unavailable ||
           state == CollaborationState::Conflict ||
           state == CollaborationState::Error;
}

} // namespace

struct CloudSessionLifecycleController::Impl {
    enum class Pending : quint8 { None, Start, End, Leave };

    struct Ports {
        std::function<quint64(const QString&, CloudSessionMode)> start;
        std::function<quint64(const QString&, const QString&)> end;
        std::function<quint64(const QString&, const QString&)> leave;
        std::function<bool(quint64)> cancel;
        std::function<void()> reconnect;
        std::function<void()> disconnect;
        std::function<QString()> serviceProjectId;
        std::function<QString()> serviceSessionId;
        std::function<QString()> serviceStateHash;
        std::function<QString()> localParticipantId;
        std::function<QString()> hostParticipantId;
        std::function<CollaborationState()> serviceState;
    } ports;

    struct DeferredFailure {
        quint64 requestId = 0;
        CloudRequestKind kind = CloudRequestKind::GetProject;
        CloudClientError error;
    };
    struct DeferredSession {
        quint64 requestId = 0;
        CloudRequestKind kind = CloudRequestKind::GetActiveSession;
        CloudSessionState state;
    };
    struct DeferredOperation {
        quint64 requestId = 0;
        CloudRequestKind kind = CloudRequestKind::GetProject;
        QString resourceId;
    };

    CloudSessionLifecycleController* q = nullptr;
    QPointer<CloudProjectClient> projects;
    QPointer<CloudProjectSyncCoordinator> synchronizer;
    QPointer<CollaborationService> service;
    QString projectId;
    QString sessionId;
    QString verifiedHash;
    CloudProjectRole role = CloudProjectRole::Viewer;
    CloudProjectStatus projectStatus = CloudProjectStatus::Archived;
    CloudSyncPhase syncPhase = CloudSyncPhase::Idle;
    CloudSessionLifecyclePhase phase =
        CloudSessionLifecyclePhase::Unbound;
    Pending pending = Pending::None;
    quint64 pendingRequestId = 0;
    bool verified = false;
    bool knownNoActiveSession = false;
    bool ending = false;
    bool left = false;
    bool issuing = false;
    CloudRequestKind issuingKind = CloudRequestKind::GetProject;
    std::optional<DeferredFailure> deferredFailure;
    std::optional<DeferredSession> deferredSession;
    std::optional<DeferredOperation> deferredOperation;
    bool lastCanStart = false;
    bool lastCanEnd = false;
    bool lastCanLeave = false;

    explicit Impl(CloudSessionLifecycleController* owner) : q(owner) {}

    QString serviceProject() const {
        return ports.serviceProjectId ? ports.serviceProjectId() : QString();
    }
    QString serviceSession() const {
        return ports.serviceSessionId ? ports.serviceSessionId() : QString();
    }
    QString serviceHash() const {
        return ports.serviceStateHash ? ports.serviceStateHash() : QString();
    }
    QString localParticipant() const {
        return ports.localParticipantId ? ports.localParticipantId() : QString();
    }
    QString hostParticipant() const {
        return ports.hostParticipantId ? ports.hostParticipantId() : QString();
    }
    CollaborationState currentServiceState() const {
        return ports.serviceState ? ports.serviceState()
                                  : CollaborationState::Unavailable;
    }

    bool exactVerifiedProjectForStart() const {
        return verified && syncPhase == CloudSyncPhase::Ready &&
               projectStatus == CloudProjectStatus::Active &&
               !projectId.isEmpty() && serviceProject() == projectId &&
               !verifiedHash.isEmpty() && serviceHash() == verifiedHash &&
               !blocksCloudRequests(currentServiceState());
    }

    bool canStart() const {
        return pending == Pending::None && !ending && !left &&
               role == CloudProjectRole::Owner && knownNoActiveSession &&
               sessionId.isEmpty() && serviceSession().isEmpty() &&
               exactVerifiedProjectForStart() && bool(ports.start);
    }

    bool hasCurrentRoom() const {
        const QString current = canonicalUuid(serviceSession());
        return !current.isEmpty() && current == canonicalUuid(sessionId) &&
               serviceProject() == projectId &&
               (currentServiceState() == CollaborationState::Synced ||
                currentServiceState() == CollaborationState::ReadOnly);
    }

    bool localIsHost() const {
        const QString local = localParticipant();
        return !local.isEmpty() && local == hostParticipant();
    }

    bool canEnd() const {
        return pending == Pending::None && !ending && !left &&
               hasCurrentRoom() &&
               currentServiceState() == CollaborationState::Synced &&
               (role == CloudProjectRole::Owner || localIsHost()) &&
               bool(ports.end);
    }

    bool canLeave() const {
        return pending == Pending::None && !ending && !left &&
               hasCurrentRoom() && bool(ports.leave);
    }

    void notifyCapabilities(bool force = false) {
        const bool start = canStart();
        const bool end = canEnd();
        const bool leave = canLeave();
        if (!force && start == lastCanStart && end == lastCanEnd &&
            leave == lastCanLeave) {
            return;
        }
        lastCanStart = start;
        lastCanEnd = end;
        lastCanLeave = leave;
        emit q->capabilitiesChanged(start, end, leave);
    }

    void setPhase(CloudSessionLifecyclePhase value) {
        if (phase != value) {
            phase = value;
            emit q->phaseChanged(phase);
        }
        notifyCapabilities();
    }

    void recomputePhase() {
        if (projectId.isEmpty()) {
            setPhase(CloudSessionLifecyclePhase::Unbound);
            return;
        }
        if (pending == Pending::Start) {
            setPhase(CloudSessionLifecyclePhase::Starting);
            return;
        }
        if (pending == Pending::End || ending) {
            setPhase(CloudSessionLifecyclePhase::Ending);
            return;
        }
        if (pending == Pending::Leave) {
            setPhase(CloudSessionLifecyclePhase::Leaving);
            return;
        }
        if (left) {
            setPhase(CloudSessionLifecyclePhase::Left);
            return;
        }
        if (hasCurrentRoom()) {
            setPhase(CloudSessionLifecyclePhase::Active);
            return;
        }
        if (!sessionId.isEmpty()) {
            setPhase(CloudSessionLifecyclePhase::Connecting);
            return;
        }
        if (verified && syncPhase == CloudSyncPhase::Ready &&
            knownNoActiveSession) {
            setPhase(CloudSessionLifecyclePhase::Ready);
            return;
        }
        setPhase(CloudSessionLifecyclePhase::WaitingForVerifiedProject);
    }

    void clearDeferred() {
        deferredFailure.reset();
        deferredSession.reset();
        deferredOperation.reset();
        issuing = false;
    }

    void cancelPending() {
        const quint64 request = pendingRequestId;
        pendingRequestId = 0;
        pending = Pending::None;
        clearDeferred();
        if (request != 0 && ports.cancel) ports.cancel(request);
    }

    void resetBinding() {
        cancelPending();
        projectId.clear();
        sessionId.clear();
        verifiedHash.clear();
        role = CloudProjectRole::Viewer;
        projectStatus = CloudProjectStatus::Archived;
        syncPhase = CloudSyncPhase::Idle;
        verified = false;
        knownNoActiveSession = false;
        ending = false;
        left = false;
        recomputePhase();
    }

    void beginIssue(Pending operation, CloudRequestKind kind) {
        pending = operation;
        pendingRequestId = 0;
        issuing = true;
        issuingKind = kind;
        deferredFailure.reset();
        deferredSession.reset();
        deferredOperation.reset();
        recomputePhase();
    }

    void finishIssue(quint64 requestId) {
        issuing = false;
        pendingRequestId = requestId;
        const auto failure = std::move(deferredFailure);
        const auto state = std::move(deferredSession);
        const auto operation = std::move(deferredOperation);
        deferredFailure.reset();
        deferredSession.reset();
        deferredOperation.reset();
        if (failure && failure->requestId == requestId)
            onRequestFailed(failure->requestId, failure->kind, failure->error);
        else if (state && state->requestId == requestId)
            onSessionState(state->requestId, state->kind, state->state);
        else if (operation && operation->requestId == requestId)
            onOperationCompleted(operation->requestId, operation->kind,
                                 operation->resourceId);
        else if (requestId == 0) {
            pending = Pending::None;
            recomputePhase();
            emit q->userNotice(
                q->tr("The collaboration request could not be started."), true);
        }
    }

    bool matches(quint64 requestId, CloudRequestKind kind,
                 Pending expected) const {
        return !issuing && pending == expected && pendingRequestId != 0 &&
               requestId == pendingRequestId &&
               ((expected == Pending::Start &&
                 kind == CloudRequestKind::StartSession) ||
                (expected == Pending::End &&
                 kind == CloudRequestKind::EndSession) ||
                (expected == Pending::Leave &&
                 kind == CloudRequestKind::LeaveSession));
    }

    void invalidateResponse(const QString& notice) {
        pendingRequestId = 0;
        pending = Pending::None;
        verified = false;
        knownNoActiveSession = false;
        sessionId.clear();
        ending = false;
        left = false;
        recomputePhase();
        emit q->userNotice(notice, true);
    }

    void onSessionState(quint64 requestId, CloudRequestKind kind,
                        const CloudSessionState& state) {
        if (issuing && kind == issuingKind) {
            deferredSession = DeferredSession{requestId, kind, state};
            return;
        }
        if (kind == CloudRequestKind::StartSession &&
            matches(requestId, kind, Pending::Start)) {
            if (state.session.projectId != projectId ||
                canonicalUuid(state.session.id).isEmpty() ||
                (state.session.status != CloudSessionStatus::Starting &&
                 state.session.status != CloudSessionStatus::Active) ||
                serviceProject() != projectId) {
                invalidateResponse(q->tr(
                    "The server returned an inconsistent session response. "
                    "Reopen the cloud project before trying again."));
                return;
            }
            pendingRequestId = 0;
            pending = Pending::None;
            sessionId = canonicalUuid(state.session.id);
            knownNoActiveSession = false;
            ending = false;
            left = false;
            recomputePhase();
            if (ports.reconnect) ports.reconnect();
            recomputePhase();
            emit q->userNotice(q->tr("Session created. Connecting…"), false);
            return;
        }
        if (kind == CloudRequestKind::LeaveSession &&
            matches(requestId, kind, Pending::Leave)) {
            if (state.session.projectId != projectId ||
                canonicalUuid(state.session.id) != canonicalUuid(sessionId)) {
                invalidateResponse(q->tr(
                    "The server returned an inconsistent leave response. "
                    "Reopen the cloud project before trying again."));
                return;
            }
            pendingRequestId = 0;
            pending = Pending::None;
            knownNoActiveSession =
                state.session.status == CloudSessionStatus::Ended;
            ending = false;
            left = true;
            if (ports.disconnect) ports.disconnect();
            recomputePhase();
            emit q->userNotice(q->tr("You left the collaboration session."),
                               false);
        }
    }

    void onOperationCompleted(quint64 requestId, CloudRequestKind kind,
                              const QString& resourceId) {
        if (issuing && kind == issuingKind) {
            deferredOperation = DeferredOperation{requestId, kind, resourceId};
            return;
        }
        if (!matches(requestId, kind, Pending::End)) return;
        if (canonicalUuid(resourceId) != canonicalUuid(sessionId)) {
            invalidateResponse(q->tr(
                "The server returned an inconsistent end-session response. "
                "Reopen the cloud project before trying again."));
            return;
        }
        pendingRequestId = 0;
        pending = Pending::None;
        ending = true;
        recomputePhase();
        // Keep the socket connected. session.ending assigns final snapshot
        // work, and session.ended is the sole successful teardown boundary.
        emit q->userNotice(
            q->tr("Ending session and saving the final project snapshot…"),
            false);
    }

    void onRequestFailed(quint64 requestId, CloudRequestKind kind,
                         const CloudClientError& error) {
        if (issuing && kind == issuingKind) {
            deferredFailure = DeferredFailure{requestId, kind, {}};
            return;
        }
        Pending expected = Pending::None;
        QString notice;
        if (kind == CloudRequestKind::StartSession) {
            expected = Pending::Start;
            notice = error.apiCode == QLatin1String("project_not_active")
                ? q->tr("This cloud project is archived or read-only. "
                        "Open an active project to start a session.")
                : q->tr("Could not start the collaboration session.");
        } else if (kind == CloudRequestKind::EndSession) {
            expected = Pending::End;
            notice = q->tr("Could not request the end of this session.");
        } else if (kind == CloudRequestKind::LeaveSession) {
            expected = Pending::Leave;
            notice = q->tr("Could not leave the collaboration session.");
        }
        if (expected == Pending::None || !matches(requestId, kind, expected))
            return;
        pendingRequestId = 0;
        pending = Pending::None;
        recomputePhase();
        emit q->userNotice(notice, true);
    }

    void onSynchronized(const QString& synchronizedProject, quint64,
                        const QString& hash, CloudProjectRole synchronizedRole,
                        CloudProjectStatus synchronizedStatus) {
        if (canonicalUuid(synchronizedProject) != projectId) return;
        const QString normalizedHash = hash.trimmed().toLower();
        if (normalizedHash.size() != 64 || serviceProject() != projectId ||
            serviceHash() != normalizedHash) {
            verified = false;
            knownNoActiveSession = false;
            recomputePhase();
            return;
        }
        verified = true;
        verifiedHash = normalizedHash;
        role = synchronizedRole;
        projectStatus = synchronizedStatus;
        knownNoActiveSession = false;
        left = false;
        recomputePhase();
    }

    void onSyncPhase(CloudSyncPhase value) {
        if (synchronizer && synchronizer->projectId() != projectId) return;
        syncPhase = value;
        if (value == CloudSyncPhase::Failed || value == CloudSyncPhase::Idle)
            knownNoActiveSession = false;
        recomputePhase();
    }

    void onNoActiveSession(const QString& checkedProject) {
        if (canonicalUuid(checkedProject) != projectId || !verified ||
            syncPhase != CloudSyncPhase::Ready) {
            return;
        }
        knownNoActiveSession = true;
        sessionId.clear();
        ending = false;
        left = false;
        recomputePhase();
    }

    void onActiveSessionCheckFailed(const QString& checkedProject) {
        if (canonicalUuid(checkedProject) != projectId) return;
        knownNoActiveSession = false;
        recomputePhase();
        emit q->userNotice(
            q->tr("Could not verify whether a collaboration session is active."),
            true);
    }

    void onRoomIdentity(const QString& roomSessionId) {
        if (serviceProject() != projectId) return;
        const QString current = canonicalUuid(roomSessionId);
        if (!current.isEmpty()) {
            sessionId = current;
            knownNoActiveSession = false;
            ending = false;
            left = false;
        }
        recomputePhase();
    }

    void onSessionEnding(const QString& roomSessionId) {
        const QString current = canonicalUuid(roomSessionId);
        if (projectId.isEmpty() || current.isEmpty() ||
            (!sessionId.isEmpty() && current != canonicalUuid(sessionId))) {
            return;
        }
        sessionId = current;
        knownNoActiveSession = false;
        ending = true;
        left = false;
        recomputePhase();
    }

    void onSessionEnded(const QString& roomSessionId) {
        const QString ended = canonicalUuid(roomSessionId);
        if (projectId.isEmpty() || ended.isEmpty() ||
            (!sessionId.isEmpty() && ended != canonicalUuid(sessionId))) {
            return;
        }
        cancelPending();
        sessionId.clear();
        knownNoActiveSession = true;
        ending = false;
        left = false;
        recomputePhase();
        emit q->userNotice(q->tr("Collaboration session ended."), false);
    }

    void onServiceState(CollaborationState state) {
        if (state == CollaborationState::SignedOut) cancelPending();
        recomputePhase();
    }
};

CloudSessionLifecycleController::CloudSessionLifecycleController(
    CloudProjectClient* projects, CloudProjectSyncCoordinator* synchronizer,
    CollaborationService* service, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(this)) {
    qRegisterMetaType<CloudSessionLifecyclePhase>();
    m_impl->projects = projects;
    m_impl->synchronizer = synchronizer;
    m_impl->service = service;

    const QPointer<CloudProjectClient> projectGuard(projects);
    const QPointer<CollaborationService> serviceGuard(service);
    m_impl->ports.start = [projectGuard](const QString& projectId,
                                         CloudSessionMode mode) {
        return projectGuard ? projectGuard->startSession(projectId, mode) : 0;
    };
    m_impl->ports.end = [projectGuard](const QString& projectId,
                                       const QString& sessionId) {
        return projectGuard ? projectGuard->endSession(projectId, sessionId) : 0;
    };
    m_impl->ports.leave = [projectGuard](const QString& projectId,
                                         const QString& sessionId) {
        return projectGuard ? projectGuard->leaveSession(projectId, sessionId)
                            : 0;
    };
    m_impl->ports.cancel = [projectGuard](quint64 requestId) {
        return projectGuard && projectGuard->cancel(requestId);
    };
    m_impl->ports.reconnect = [serviceGuard] {
        if (serviceGuard) serviceGuard->reconnectNow();
    };
    m_impl->ports.disconnect = [serviceGuard] {
        if (serviceGuard) serviceGuard->disconnectFromProject();
    };
    m_impl->ports.serviceProjectId = [serviceGuard] {
        return serviceGuard ? serviceGuard->projectId() : QString();
    };
    m_impl->ports.serviceSessionId = [serviceGuard] {
        return serviceGuard ? serviceGuard->sessionId() : QString();
    };
    m_impl->ports.serviceStateHash = [serviceGuard] {
        return serviceGuard ? serviceGuard->bootstrapStateHash() : QString();
    };
    m_impl->ports.localParticipantId = [serviceGuard] {
        return serviceGuard ? serviceGuard->localParticipantId() : QString();
    };
    m_impl->ports.hostParticipantId = [serviceGuard] {
        return serviceGuard ? serviceGuard->hostParticipantId() : QString();
    };
    m_impl->ports.serviceState = [serviceGuard] {
        return serviceGuard ? serviceGuard->state()
                            : CollaborationState::Unavailable;
    };

    if (projects) {
        connect(projects, &CloudProjectClient::sessionStateReceived, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudSessionState& state) {
                    m_impl->onSessionState(requestId, kind, state);
                });
        connect(projects, &CloudProjectClient::operationCompleted, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const QString& resourceId) {
                    m_impl->onOperationCompleted(requestId, kind, resourceId);
                });
        connect(projects, &CloudProjectClient::requestFailed, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudClientError& error) {
                    m_impl->onRequestFailed(requestId, kind, error);
                });
    }
    if (synchronizer) {
        connect(synchronizer, &CloudProjectSyncCoordinator::phaseChanged, this,
                [this](CloudSyncPhase phase) { m_impl->onSyncPhase(phase); });
        connect(synchronizer,
                &CloudProjectSyncCoordinator::synchronizedProject, this,
                [this](const QString& projectId, quint64 serverSequence,
                       const QString& hash, CloudProjectRole role,
                       CloudProjectStatus status) {
                    m_impl->onSynchronized(projectId, serverSequence, hash,
                                           role, status);
                });
        connect(synchronizer,
                &CloudProjectSyncCoordinator::noActiveSession, this,
                [this](const QString& projectId) {
                    m_impl->onNoActiveSession(projectId);
                });
        connect(synchronizer,
                &CloudProjectSyncCoordinator::activeSessionCheckFailed, this,
                [this](const QString& projectId, const QString&, bool) {
                    m_impl->onActiveSessionCheckFailed(projectId);
                });
    }
    if (service) {
        connect(service, &CollaborationService::projectChanged, this,
                [this](const QString& projectId) {
                    if (!m_impl->projectId.isEmpty() &&
                        canonicalUuid(projectId) != m_impl->projectId)
                        m_impl->resetBinding();
                    else
                        m_impl->recomputePhase();
                });
        connect(service, &CollaborationService::roomIdentityChanged, this,
                [this](const QString& sessionId, const QString&,
                       const QString&) {
                    m_impl->onRoomIdentity(sessionId);
                });
        connect(service, &CollaborationService::liveSessionEnding, this,
                [this](const QString& sessionId) {
                    m_impl->onSessionEnding(sessionId);
                });
        connect(service, &CollaborationService::liveSessionEnded, this,
                [this](const QString& sessionId) {
                    m_impl->onSessionEnded(sessionId);
                });
        connect(service, &CollaborationService::stateChanged, this,
                [this](CollaborationState state, const QString&) {
                    m_impl->onServiceState(state);
                });
    }
}

CloudSessionLifecycleController::~CloudSessionLifecycleController() {
    m_impl->cancelPending();
}

void CloudSessionLifecycleController::bindProject(const QString& projectId) {
    const QString normalized = canonicalUuid(projectId);
    if (normalized.isEmpty()) {
        m_impl->resetBinding();
        return;
    }
    if (normalized == m_impl->projectId) {
        m_impl->recomputePhase();
        return;
    }
    m_impl->resetBinding();
    m_impl->projectId = normalized;
    m_impl->syncPhase = CloudSyncPhase::Idle;
    m_impl->recomputePhase();
}

void CloudSessionLifecycleController::clearBinding() {
    m_impl->resetBinding();
}

QString CloudSessionLifecycleController::projectId() const {
    return m_impl->projectId;
}

QString CloudSessionLifecycleController::sessionId() const {
    return m_impl->sessionId;
}

CloudProjectRole CloudSessionLifecycleController::role() const noexcept {
    return m_impl->role;
}

CloudProjectStatus
CloudSessionLifecycleController::projectStatus() const noexcept {
    return m_impl->projectStatus;
}

CloudSessionLifecyclePhase
CloudSessionLifecycleController::phase() const noexcept {
    return m_impl->phase;
}

bool CloudSessionLifecycleController::canStartSession() const {
    return m_impl->canStart();
}

bool CloudSessionLifecycleController::canEndSession() const {
    return m_impl->canEnd();
}

bool CloudSessionLifecycleController::canLeaveSession() const {
    return m_impl->canLeave();
}

bool CloudSessionLifecycleController::startSession() {
    if (!m_impl->canStart()) return false;
    m_impl->beginIssue(Impl::Pending::Start,
                       CloudRequestKind::StartSession);
    const quint64 requestId = m_impl->ports.start(
        m_impl->projectId, CloudSessionMode::Independent);
    m_impl->finishIssue(requestId);
    return requestId != 0;
}

bool CloudSessionLifecycleController::endSession() {
    if (!m_impl->canEnd()) return false;
    const QString currentSession = canonicalUuid(m_impl->serviceSession());
    if (currentSession.isEmpty() || currentSession != m_impl->sessionId)
        return false;
    m_impl->beginIssue(Impl::Pending::End, CloudRequestKind::EndSession);
    const quint64 requestId =
        m_impl->ports.end(m_impl->projectId, currentSession);
    m_impl->finishIssue(requestId);
    return requestId != 0;
}

bool CloudSessionLifecycleController::leaveSession() {
    if (!m_impl->canLeave()) return false;
    const QString currentSession = canonicalUuid(m_impl->serviceSession());
    if (currentSession.isEmpty() || currentSession != m_impl->sessionId)
        return false;
    m_impl->beginIssue(Impl::Pending::Leave,
                       CloudRequestKind::LeaveSession);
    const quint64 requestId =
        m_impl->ports.leave(m_impl->projectId, currentSession);
    m_impl->finishIssue(requestId);
    return requestId != 0;
}

bool checkCloudSessionLifecycleControllerForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString projectId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QString otherProjectId =
        QStringLiteral("22222222-2222-4222-8222-222222222222");
    const QString sessionId =
        QStringLiteral("33333333-3333-4333-8333-333333333333");
    const QString secondSessionId =
        QStringLiteral("44444444-4444-4444-8444-444444444444");
    const QString participantId =
        QStringLiteral("55555555-5555-4555-8555-555555555555");
    const QString hash(64, QLatin1Char('a'));

    CloudSessionLifecycleController lifecycle(nullptr, nullptr, nullptr);
    auto& impl = *lifecycle.m_impl;
    QString serviceProject = projectId;
    QString serviceSession;
    QString serviceHash = hash;
    QString localParticipant = participantId;
    QString hostParticipant = participantId;
    CollaborationState serviceState = CollaborationState::LocalOnly;
    quint64 nextRequest = 10;
    int starts = 0;
    int ends = 0;
    int leaves = 0;
    int reconnects = 0;
    int disconnects = 0;
    int cancels = 0;
    CloudSessionMode requestedMode = CloudSessionMode::FollowHost;

    impl.ports.start = [&](const QString& requestedProject,
                           CloudSessionMode mode) {
        if (requestedProject == projectId) ++starts;
        requestedMode = mode;
        return ++nextRequest;
    };
    impl.ports.end = [&](const QString& requestedProject,
                         const QString& requestedSession) {
        if (requestedProject == projectId &&
            requestedSession == serviceSession)
            ++ends;
        return ++nextRequest;
    };
    impl.ports.leave = [&](const QString& requestedProject,
                           const QString& requestedSession) {
        if (requestedProject == projectId &&
            requestedSession == serviceSession)
            ++leaves;
        return ++nextRequest;
    };
    impl.ports.cancel = [&](quint64) {
        ++cancels;
        return true;
    };
    impl.ports.reconnect = [&] { ++reconnects; };
    impl.ports.disconnect = [&] {
        ++disconnects;
        serviceState = CollaborationState::LocalOnly;
        serviceSession.clear();
    };
    impl.ports.serviceProjectId = [&] { return serviceProject; };
    impl.ports.serviceSessionId = [&] { return serviceSession; };
    impl.ports.serviceStateHash = [&] { return serviceHash; };
    impl.ports.localParticipantId = [&] { return localParticipant; };
    impl.ports.hostParticipantId = [&] { return hostParticipant; };
    impl.ports.serviceState = [&] { return serviceState; };

    lifecycle.bindProject(projectId);
    impl.syncPhase = CloudSyncPhase::Ready;
    impl.onSynchronized(projectId, 0, hash, CloudProjectRole::Owner,
                        CloudProjectStatus::Archived);
    impl.onNoActiveSession(projectId);
    if (lifecycle.canStartSession())
        return fail(QStringLiteral("archived project allowed session start"));
    impl.onSynchronized(projectId, 0, hash, CloudProjectRole::Owner,
                        CloudProjectStatus::Active);
    impl.onNoActiveSession(projectId);
    if (!lifecycle.canStartSession() || lifecycle.canEndSession() ||
        lifecycle.canLeaveSession()) {
        return fail(QStringLiteral("verified owner start gate is incorrect"));
    }
    if (!lifecycle.startSession() || starts != 1 ||
        requestedMode != CloudSessionMode::Independent ||
        lifecycle.startSession()) {
        return fail(QStringLiteral("start request was not bounded"));
    }
    CloudSessionState active;
    active.session.id = sessionId;
    active.session.projectId = projectId;
    active.session.status = CloudSessionStatus::Active;
    impl.onSessionState(999, CloudRequestKind::StartSession, active);
    if (reconnects != 0)
        return fail(QStringLiteral("stale start response was accepted"));

    CloudSessionState mismatched = active;
    mismatched.session.projectId = otherProjectId;
    impl.onSessionState(nextRequest, CloudRequestKind::StartSession,
                        mismatched);
    if (reconnects != 0 || lifecycle.canStartSession()) {
        return fail(QStringLiteral("mismatched start response was trusted"));
    }

    lifecycle.bindProject(otherProjectId);
    lifecycle.bindProject(projectId);
    impl.syncPhase = CloudSyncPhase::Ready;
    impl.onSynchronized(projectId, 0, hash, CloudProjectRole::Owner,
                        CloudProjectStatus::Active);
    impl.onNoActiveSession(projectId);
    if (!lifecycle.startSession())
        return fail(QStringLiteral("start retry was not enabled"));
    const quint64 startRequest = nextRequest;
    impl.onSessionState(startRequest, CloudRequestKind::StartSession, active);
    if (reconnects != 1 ||
        lifecycle.phase() != CloudSessionLifecyclePhase::Connecting) {
        return fail(QStringLiteral("accepted start did not reconnect"));
    }
    serviceSession = sessionId;
    serviceState = CollaborationState::Synced;
    impl.onRoomIdentity(sessionId);
    if (!lifecycle.canEndSession() || !lifecycle.canLeaveSession() ||
        lifecycle.canStartSession()) {
        return fail(QStringLiteral("active host capabilities are incorrect"));
    }

    if (!lifecycle.endSession() || ends != 1 || lifecycle.endSession())
        return fail(QStringLiteral("end request was not bounded"));
    impl.onOperationCompleted(nextRequest + 1, CloudRequestKind::EndSession,
                              sessionId);
    if (lifecycle.phase() != CloudSessionLifecyclePhase::Ending)
        return fail(QStringLiteral("stale end acknowledgement changed state"));
    impl.onOperationCompleted(nextRequest, CloudRequestKind::EndSession,
                              sessionId);
    if (lifecycle.phase() != CloudSessionLifecyclePhase::Ending ||
        lifecycle.canLeaveSession()) {
        return fail(QStringLiteral("accepted end did not await WebSocket"));
    }
    impl.onSessionEnding(sessionId);
    serviceSession.clear();
    serviceState = CollaborationState::LocalOnly;
    impl.onSessionEnded(sessionId);
    if (!lifecycle.canStartSession() ||
        lifecycle.phase() != CloudSessionLifecyclePhase::Ready) {
        return fail(QStringLiteral("session.ended did not restore owner start"));
    }

    if (!lifecycle.startSession())
        return fail(QStringLiteral("second session could not start"));
    active.session.id = secondSessionId;
    impl.onSessionState(nextRequest, CloudRequestKind::StartSession, active);
    serviceSession = secondSessionId;
    serviceState = CollaborationState::Synced;
    impl.onRoomIdentity(secondSessionId);
    if (!lifecycle.leaveSession() || leaves != 1 || lifecycle.leaveSession())
        return fail(QStringLiteral("leave request was not bounded"));
    CloudSessionState leftState = active;
    leftState.session.id = secondSessionId;
    impl.onSessionState(nextRequest, CloudRequestKind::LeaveSession, leftState);
    if (disconnects != 1 ||
        lifecycle.phase() != CloudSessionLifecyclePhase::Left ||
        lifecycle.canStartSession() || lifecycle.canEndSession() ||
        lifecycle.canLeaveSession()) {
        return fail(QStringLiteral("leave did not disconnect locally"));
    }

    lifecycle.bindProject(otherProjectId);
    lifecycle.bindProject(projectId);
    impl.syncPhase = CloudSyncPhase::Ready;
    impl.onSynchronized(projectId, 0, hash, CloudProjectRole::Editor,
                        CloudProjectStatus::Active);
    serviceSession = sessionId;
    serviceState = CollaborationState::Synced;
    impl.onRoomIdentity(sessionId);
    if (!lifecycle.canEndSession())
        return fail(QStringLiteral("verified editor host could not end"));
    if (!lifecycle.endSession())
        return fail(QStringLiteral("logout cancellation setup failed"));
    serviceState = CollaborationState::SignedOut;
    impl.onServiceState(CollaborationState::SignedOut);
    if (cancels != 1 || lifecycle.canEndSession())
        return fail(QStringLiteral("logout did not cancel lifecycle request"));
    impl.onOperationCompleted(nextRequest, CloudRequestKind::EndSession,
                              sessionId);
    if (lifecycle.phase() == CloudSessionLifecyclePhase::Ending)
        return fail(QStringLiteral("stale post-logout response was accepted"));

    serviceState = CollaborationState::Synced;
    impl.onServiceState(serviceState);
    if (!lifecycle.endSession())
        return fail(QStringLiteral("binding cancellation setup failed"));
    const quint64 clearedRequest = nextRequest;
    lifecycle.clearBinding();
    if (cancels != 2 ||
        lifecycle.phase() != CloudSessionLifecyclePhase::Unbound) {
        return fail(QStringLiteral("binding clear did not cancel its request"));
    }
    impl.onOperationCompleted(clearedRequest, CloudRequestKind::EndSession,
                              sessionId);
    if (lifecycle.phase() != CloudSessionLifecyclePhase::Unbound)
        return fail(QStringLiteral("post-clear response changed lifecycle"));

    serviceSession.clear();
    serviceState = CollaborationState::LocalOnly;
    lifecycle.bindProject(projectId);
    impl.syncPhase = CloudSyncPhase::Ready;
    impl.onSynchronized(projectId, 0, hash, CloudProjectRole::Owner,
                        CloudProjectStatus::Active);
    impl.onNoActiveSession(projectId);
    impl.ports.start = [&](const QString&, CloudSessionMode) {
        const quint64 requestId = ++nextRequest;
        impl.onRequestFailed(requestId, CloudRequestKind::StartSession, {});
        return requestId;
    };
    if (!lifecycle.startSession() || !lifecycle.canStartSession() ||
        lifecycle.phase() != CloudSessionLifecyclePhase::Ready) {
        return fail(QStringLiteral("synchronous REST failure left a request stuck"));
    }
    lifecycle.clearBinding();
    return true;
}

} // namespace collab
