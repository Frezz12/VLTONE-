#pragma once

#include "AssetCache.hpp"
#include "CloudAssetTransferManager.hpp"

#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

namespace collab {

/// One immutable, already-closed recording file. All UUIDs are supplied by
/// the caller and remain stable across cache import, resumable retries and
/// recovery. sourcePath is local-only and is never copied into AssetRef.
struct ClosedRecordingAsset {
    QString projectId;
    QString uploadId;
    QString assetId;
    QString sourcePath;
    QString displayName;
    QString contentType = QStringLiteral("audio/wav");
    QString codec;
    double sampleRate = 0.0;
    quint32 channels = 0;
    quint64 frames = 0;
};

enum class CloudRecordingAssetState : quint8 {
    Idle,
    Importing,
    Uploading,
    Ready,
    Failed,
    Cancelled,
};

enum class CloudRecordingAssetItemState : quint8 {
    Importing,
    Uploading,
    Ready,
    Failed,
    Cancelled,
};

struct CloudRecordingAssetResult {
    QString projectId;
    QString uploadId;
    QString localPath;
    daw::AssetRef asset;
};

/// A detached, memory-only description of work that can be retried or handed
/// to a later crash-recovery layer. No settings or files are written here.
struct CloudRecordingAssetRecoveryItem {
    ClosedRecordingAsset recording;
    CloudRecordingAssetItemState state =
        CloudRecordingAssetItemState::Importing;
    QString cachedPath;
    QString sha256;
    quint64 byteSize = 0;
    QString safeMessage;
    bool retryable = false;
};

/// Narrow asynchronous boundary. The production adapter imports through
/// AssetCache on a worker and delegates resumable transfer to
/// CloudAssetTransferManager; deterministic tests may inject a synchronous
/// fake without constructing a network stack.
class CloudRecordingAssetPort : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    ~CloudRecordingAssetPort() override = default;

    virtual quint64 importFile(const daw::AssetRef& expected,
                               const QString& sourcePath) = 0;
    virtual bool cancelImport(quint64 requestId) = 0;
    virtual quint64 uploadAsset(const CloudAssetUploadInput& input) = 0;
    virtual bool retryUpload(quint64 transferId) = 0;
    virtual bool cancelUpload(quint64 transferId) = 0;

signals:
    void importCompleted(quint64 requestId,
                         const collab::AssetCacheResult& result);
    void importFailed(quint64 requestId, const QString& safeMessage,
                      bool retryable);
    void assetUploadCompleted(
        quint64 transferId,
        const collab::CloudAssetUploadResult& result);
    void transferFailed(quint64 transferId,
                        collab::CloudTransferKind kind,
                        const collab::CloudTransferError& error);
    void unavailable();
};

/// Prepares one atomic recording asset set without mutating a project.
///
/// Each WAV is first copied into the content-addressed AssetCache on a worker.
/// The verified cached copy is then uploaded. A ready AssetRef is emitted only
/// after the backend complete response arrives through assetUploadCompleted.
class CloudRecordingAssetCoordinator final : public QObject {
    Q_OBJECT
public:
    CloudRecordingAssetCoordinator(CloudAssetTransferManager* transfers,
                                   AssetCache* cache,
                                   QObject* parent = nullptr);
    explicit CloudRecordingAssetCoordinator(CloudRecordingAssetPort* port,
                                            QObject* parent = nullptr);
    ~CloudRecordingAssetCoordinator() override;

    /// Replaces any active set and returns its non-zero generation. The set is
    /// bounded to the V1 maximum of eight simultaneously recorded tracks.
    quint64 begin(const QVector<ClosedRecordingAsset>& recordings);

    /// Retry one failed item. Import failures restart cache import; transfer
    /// failures first resume the existing transfer and fall back to a fresh
    /// prepare with the same stable uploadId when the transfer no longer exists.
    bool retry(const QString& uploadId);
    void retryFailed();
    void cancel();

    quint64 generation() const noexcept;
    CloudRecordingAssetState state() const noexcept;
    QVector<CloudRecordingAssetRecoveryItem> recoveryItems() const;

signals:
    void stateChanged(quint64 generation,
                      collab::CloudRecordingAssetState state);
    void progressChanged(quint64 generation, qsizetype completed,
                         qsizetype total);
    void assetReady(quint64 generation,
                    const collab::CloudRecordingAssetResult& result);
    void batchReady(
        quint64 generation,
        const QVector<collab::CloudRecordingAssetResult>& results);
    void itemFailed(quint64 generation, const QString& uploadId,
                    const QString& safeMessage, bool retryable);
    void batchFailed(quint64 generation, const QString& safeMessage,
                     bool retryable);
    void batchCancelled(quint64 generation);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend bool checkCloudRecordingAssetCoordinatorForTest(QString* error);
};

bool checkCloudRecordingAssetCoordinatorForTest(QString* error = nullptr);

} // namespace collab

Q_DECLARE_METATYPE(collab::CloudRecordingAssetState)
Q_DECLARE_METATYPE(collab::CloudRecordingAssetItemState)
Q_DECLARE_METATYPE(collab::CloudRecordingAssetResult)
Q_DECLARE_METATYPE(collab::CloudRecordingAssetRecoveryItem)

