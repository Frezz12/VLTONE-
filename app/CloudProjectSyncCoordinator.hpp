#pragma once

#include "CloudProjectClient.hpp"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <memory>

namespace collab {

class CloudAssetTransferManager;
class CollaborationCommandBridge;
class CollaborationService;

enum class CloudSyncPhase : quint8 {
    Idle,
    FetchingBootstrap,
    DownloadingSnapshot,
    ReplayingOperations,
    ReconcilingPending,
    CheckingLiveSession,
    Ready,
    Failed,
};

enum class CloudSyncErrorCode : quint8 {
    InvalidInput,
    BootstrapFailed,
    SnapshotDownloadFailed,
    SnapshotInvalid,
    ReplayRejected,
    FieldHeadsMismatch,
    StaleResult,
    InstallRejected,
};

struct CloudSyncError {
    CloudSyncErrorCode code = CloudSyncErrorCode::BootstrapFailed;
    QString safeMessage;
    bool retryable = false;
};

/// Coordinates the trusted cloud bootstrap boundary:
/// REST metadata -> verified snapshot bytes -> deterministic reducer replay ->
/// exact field-head check -> CommandGateway install -> WebSocket reconnect.
///
/// Snapshot file I/O, parsing, replay and hashing run on a worker-pool thread.
/// Only the final immutable materialization is installed on the UI thread.
class CloudProjectSyncCoordinator final : public QObject {
    Q_OBJECT
public:
    CloudProjectSyncCoordinator(CloudProjectClient* projects,
                                CloudAssetTransferManager* transfers,
                                CollaborationCommandBridge* bridge,
                                CollaborationService* service,
                                QObject* parent = nullptr);
    ~CloudProjectSyncCoordinator() override;

    /// Starts a full canonical bootstrap. `connectIfLive` first checks for an
    /// active room; a project without one stays materialized and read-only.
    quint64 synchronize(const QString& projectId,
                        bool connectIfLive = true);
    void cancel();

    CloudSyncPhase phase() const noexcept;
    QString projectId() const;
    quint64 generation() const noexcept;

signals:
    void phaseChanged(collab::CloudSyncPhase phase);
    void synchronizedProject(const QString& projectId,
                             quint64 serverSequence,
                             const QString& canonicalStateHash,
                             collab::CloudProjectRole role);
    /// Emitted only for an authenticated online bootstrap. Installation never
    /// waits for these bytes; a bounded hydrator may fill AssetCache while the
    /// project is already visible with silent placeholders.
    void projectAssetsDiscovered(const QString& projectId,
                                 const QList<daw::AssetRef>& assets);
    void noActiveSession(const QString& projectId);
    /// The document is verified and remains open, but REST could not prove
    /// whether a live room exists. Callers must keep Start disabled; only a
    /// genuine endpoint 404 emits noActiveSession.
    void activeSessionCheckFailed(const QString& projectId,
                                  const QString& safeMessage,
                                  bool retryable);
    void synchronizationFailed(quint64 generation,
                               const collab::CloudSyncError& error);
    void snapshotUploadStarted(const QString& requestId, int attempt,
                               quint64 targetServerSequence);
    void snapshotUploadCompleted(const QString& requestId,
                                 quint64 targetServerSequence);
    void snapshotUploadFailed(const QString& requestId,
                              quint64 targetServerSequence,
                              const QString& safeMessage,
                              bool retryable);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

bool checkCloudProjectSyncCoordinatorForTest(QString* error = nullptr);

} // namespace collab

Q_DECLARE_METATYPE(collab::CloudSyncPhase)
Q_DECLARE_METATYPE(collab::CloudSyncError)
