#pragma once

#include "collaboration/ProjectCommand.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace account { class Service; }

namespace collab {

enum class CloudRequestKind : quint8 {
    CreateProject,
    ListProjects,
    GetProject,
    ArchiveProject,
    PublishProject,
    BootstrapProject,
    LookupOperation,
    GetActiveSession,
    StartSession,
    JoinSession,
    LeaveSession,
    EndSession,
    HandoffSession,
    ListMembers,
    PutMember,
    RemoveMember,
    TransferOwnership,
    ListInvites,
    CreateInvite,
    RevokeInvite,
    AcceptInvite,
    AcquireRecordingLease,
    RenewRecordingLease,
    ReleaseRecordingLease,
};

enum class CloudClientErrorCode : quint8 {
    InvalidInput,
    Unauthenticated,
    Offline,
    UnsafeOrigin,
    NetworkFailure,
    Timeout,
    Cancelled,
    RedirectRejected,
    ResponseTooLarge,
    UnexpectedStatus,
    InvalidJson,
    InvalidResponse,
    BootstrapGap,
    BootstrapMismatch,
};

struct CloudClientError {
    CloudClientErrorCode code = CloudClientErrorCode::InvalidResponse;
    int httpStatus = 0;
    QString apiCode;
    QString safeMessage;
    QString serverRequestId;
    bool retryable = false;
};

enum class CloudProjectStatus : quint8 {
    Uploading,
    Active,
    ReadOnly,
    Conflict,
    Archived,
};

enum class CloudProjectRole : quint8 { Owner, Editor, Viewer };
enum class CloudMemberRole : quint8 { Editor, Viewer };
enum class CloudSessionMode : quint8 {
    Independent,
    FollowHost,
    Synchronized,
};
enum class CloudSessionStatus : quint8 { Starting, Active, Ending, Ended };
enum class CloudProjectLeaseKind : quint8 { Record };

struct CloudProject {
    QString id;
    QString ownerUserId;
    QString title;
    CloudProjectStatus status = CloudProjectStatus::Uploading;
    int formatVersion = 0;
    QString engineVersion;
    QString minimumAppVersion;
    quint64 headSequence = 0;
    quint64 snapshotSequence = 0;
    QDateTime createdAt;
    QDateTime updatedAt;
    QDateTime archivedAt;
};

struct CloudProjectView {
    CloudProject project;
    CloudProjectRole role = CloudProjectRole::Viewer;
};

struct CreateCloudProjectInput {
    QString title;
    QString engineVersion;
    QString minimumAppVersion;
    int formatVersion = 7;
};

struct CloudSnapshotDescriptor {
    QString id;
    QString projectId;
    quint64 sequence = 0;
    QString blobId;
    int schemaVersion = 0;
    QString createdBy;
    QDateTime createdAt;
    QStringList assetIds;
};

struct CloudProjectOperation {
    QString projectId;
    quint64 serverSequence = 0;
    QString actorUserId;
    QString actorDeviceId;
    daw::collab::ProjectCommand command;
    QDateTime createdAt;
};

/// Authoritative durability probe for one idempotent operation. `headSequence`
/// and `operation` are read in the same server transaction, so an absent result
/// is usable as a typed recovery-planner proof only for that exact head.
struct CloudOperationLookup {
    QString projectId;
    QString operationId;
    quint64 headSequence = 0;
    std::optional<CloudProjectOperation> operation;

    bool found() const noexcept { return operation.has_value(); }
};

struct CloudProjectFieldHead {
    QString projectId;
    QString fieldKey;
    quint64 headSequence = 0;
    QString operationId;
    QDateTime updatedAt;
};

/// Canonical bootstrap metadata. Snapshot bytes remain the responsibility of
/// the asset-transfer layer; operations are already strict typed commands.
struct CloudProjectBootstrap {
    CloudProject project;
    CloudProjectRole role = CloudProjectRole::Viewer;
    std::optional<CloudSnapshotDescriptor> snapshot;
    QVector<CloudProjectOperation> operations;
    QVector<CloudProjectFieldHead> fieldHeads;
    quint64 requestedAfterSequence = 0;
    quint64 replayBaseSequence = 0;
    quint64 headSequence = 0;
};

struct CloudProjectMember {
    QString projectId;
    QString userId;
    CloudMemberRole role = CloudMemberRole::Viewer;
    int colorIndex = 0;
    QString invitedBy;
    QDateTime joinedAt;
    QDateTime updatedAt;
};

struct PutCloudProjectMemberInput {
    CloudMemberRole role = CloudMemberRole::Viewer;
    int colorIndex = 0;
};

struct CloudProjectInvite {
    QString id;
    QString projectId;
    QString invitedBy;
    CloudMemberRole role = CloudMemberRole::Viewer;
    QDateTime expiresAt;
    QString acceptedBy;
    QDateTime acceptedAt;
    QDateTime revokedAt;
    QDateTime createdAt;
};

struct CreateCloudProjectInviteInput {
    CloudMemberRole role = CloudMemberRole::Viewer;
    QString targetEmail;
    qint64 expiresInSeconds = 7 * 24 * 60 * 60;
};

struct CreatedCloudProjectInvite {
    CloudProjectInvite invite;
    /// One-use secret returned only by create. Never log or persist it.
    QString oneTimeToken;
};

struct CloudOwnershipTransfer {
    CloudProject project;
    QString previousOwnerUserId;
    QString newOwnerUserId;
};

struct CloudLiveSession {
    QString id;
    QString projectId;
    QString createdBy;
    QString hostMemberId;
    CloudSessionMode mode = CloudSessionMode::Independent;
    CloudSessionStatus status = CloudSessionStatus::Starting;
    quint64 version = 0;
    QDateTime createdAt;
    QDateTime startedAt;
    QDateTime updatedAt;
    QDateTime endedAt;
};

struct CloudSessionMember {
    QString id;
    QString sessionId;
    QString userId;
    QString deviceId;
    QString desktopSessionId;
    QDateTime joinedAt;
    QDateTime lastSeenAt;
    QDateTime leftAt;
};

struct CloudSessionState {
    CloudLiveSession session;
    QVector<CloudSessionMember> members;
};

struct CloudProjectTrackLease {
    QString id;
    QString projectId;
    QString sessionId;
    QString trackId;
    CloudProjectLeaseKind kind = CloudProjectLeaseKind::Record;
    QString holderMemberId;
    QDateTime acquiredAt;
    QDateTime renewedAt;
    QDateTime expiresAt;
};

/// Authenticated, same-origin REST boundary for collaboration metadata. It
/// never downloads snapshot/audio bytes and never exposes raw JSON to UI code.
class CloudProjectClient final : public QObject {
    Q_OBJECT
public:
    explicit CloudProjectClient(account::Service* account,
                                QNetworkAccessManager* network = nullptr,
                                QObject* parent = nullptr);
    ~CloudProjectClient() override;

    int requestTimeoutMs() const;
    void setRequestTimeoutMs(int timeoutMs);
    QString currentUserId() const;

    quint64 createProject(const CreateCloudProjectInput& input);
    quint64 listProjects();
    quint64 getProject(const QString& projectId);
    quint64 archiveProject(const QString& projectId);
    quint64 publishProject(const QString& projectId);

    /// Starts a new single-generation bootstrap. Any older in-flight
    /// bootstrap is cancelled silently and its callbacks can no longer emit.
    quint64 bootstrapProject(const QString& projectId,
                             quint64 afterSequence = 0,
                             int pageLimit = 500);

    /// Checks the retained durable log directly. Unlike snapshot
    /// appliedOperationIds, this remains authoritative after later field
    /// writers replace every field touched by the requested operation.
    quint64 lookupOperation(const QString& projectId,
                            const QString& operationId);

    quint64 getActiveSession(const QString& projectId);
    quint64 startSession(
        const QString& projectId,
        CloudSessionMode mode = CloudSessionMode::Independent);
    quint64 joinSession(const QString& projectId, const QString& sessionId);
    quint64 leaveSession(const QString& projectId, const QString& sessionId);
    quint64 endSession(const QString& projectId, const QString& sessionId);
    quint64 handoffSession(const QString& projectId,
                           const QString& sessionId,
                           const QString& targetMemberId);

    /// Acquires the exclusive recording lease for one shared track. A zero
    /// TTL asks the server to use its configured default; explicit values are
    /// limited to the protocol range of 5..120 seconds.
    quint64 acquireRecordingLease(const QString& projectId,
                                  const QString& sessionId,
                                  const QString& trackId,
                                  int ttlSeconds = 0);
    quint64 renewRecordingLease(const QString& projectId,
                                const QString& sessionId,
                                const QString& leaseId,
                                int ttlSeconds = 0);
    quint64 releaseRecordingLease(const QString& projectId,
                                  const QString& sessionId,
                                  const QString& leaseId);

    quint64 listMembers(const QString& projectId);
    quint64 putMember(const QString& projectId, const QString& userId,
                      const PutCloudProjectMemberInput& input);
    quint64 removeMember(const QString& projectId, const QString& userId);
    quint64 transferOwnership(const QString& projectId,
                              const QString& targetUserId);

    quint64 listInvites(const QString& projectId);
    quint64 createInvite(const QString& projectId,
                         const CreateCloudProjectInviteInput& input);
    quint64 revokeInvite(const QString& projectId, const QString& inviteId);
    quint64 acceptInvite(const QString& oneTimeToken);

    bool cancel(quint64 requestId);
    void cancelAll();

signals:
    void projectsListed(quint64 requestId,
                        const QVector<collab::CloudProjectView>& projects);
    void projectReceived(quint64 requestId, collab::CloudRequestKind kind,
                         const collab::CloudProjectView& project);
    void bootstrapCompleted(quint64 requestId,
                            const collab::CloudProjectBootstrap& bootstrap);
    void operationLookupReceived(
        quint64 requestId,
        const collab::CloudOperationLookup& lookup);
    void sessionStateReceived(quint64 requestId,
                              collab::CloudRequestKind kind,
                              const collab::CloudSessionState& state);
    void leaseReceived(quint64 requestId, collab::CloudRequestKind kind,
                       const collab::CloudProjectTrackLease& lease);
    void membersListed(quint64 requestId,
                       const QVector<collab::CloudProjectMember>& members);
    void memberSaved(quint64 requestId,
                     const collab::CloudProjectMember& member);
    void ownershipTransferred(
        quint64 requestId,
        const collab::CloudOwnershipTransfer& transfer);
    void invitesListed(
        quint64 requestId,
        const QVector<collab::CloudProjectInvite>& invites);
    void inviteCreated(quint64 requestId,
                       const collab::CreatedCloudProjectInvite& invite);
    void inviteAccepted(quint64 requestId,
                        const collab::CloudProjectView& project);
    void operationCompleted(quint64 requestId,
                            collab::CloudRequestKind kind,
                            const QString& resourceId);
    void requestFailed(quint64 requestId, collab::CloudRequestKind kind,
                       const collab::CloudClientError& error);

private:
    struct Credentials {
        QString apiOrigin;
        QByteArray bearerToken;
        QString userId;
        QString deviceId;
        bool authenticated = false;
        bool offline = false;
    };
    using CredentialProvider = std::function<Credentials()>;

    CloudProjectClient(CredentialProvider credentials,
                       QNetworkAccessManager* network,
                       QObject* parent);

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend bool checkCloudProjectClientForTest(QString* error);
};

bool checkCloudProjectClientForTest(QString* error = nullptr);

} // namespace collab

Q_DECLARE_METATYPE(collab::CloudRequestKind)
Q_DECLARE_METATYPE(collab::CloudClientError)
Q_DECLARE_METATYPE(collab::CloudProjectView)
Q_DECLARE_METATYPE(collab::CloudProjectBootstrap)
Q_DECLARE_METATYPE(collab::CloudOperationLookup)
Q_DECLARE_METATYPE(collab::CloudProjectMember)
Q_DECLARE_METATYPE(collab::CloudProjectInvite)
Q_DECLARE_METATYPE(collab::CreatedCloudProjectInvite)
Q_DECLARE_METATYPE(collab::CloudOwnershipTransfer)
Q_DECLARE_METATYPE(collab::CloudSessionState)
Q_DECLARE_METATYPE(collab::CloudProjectTrackLease)
