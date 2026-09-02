#pragma once

#include "AssetCache.hpp"

#include <QDateTime>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

class QNetworkAccessManager;

namespace account { class Service; }

namespace collab {

enum class CloudTransferKind : quint8 {
    AssetUpload,
    SnapshotUpload,
    AssetDownload,
    SnapshotDownload,
    AbortUpload,
};

enum class CloudTransferState : quint8 {
    Hashing,
    Preparing,
    Uploading,
    Completing,
    Downloading,
    Verifying,
    Ready,
    Failed,
    Cancelled,
};

enum class CloudTransferErrorCode : quint8 {
    InvalidInput,
    Unauthenticated,
    Offline,
    UnsafeOrigin,
    FileUnavailable,
    FileReadFailure,
    FileWriteFailure,
    IntegrityMismatch,
    NetworkFailure,
    Timeout,
    Cancelled,
    RedirectRejected,
    ResponseTooLarge,
    UnexpectedStatus,
    InvalidJson,
    InvalidResponse,
    DelegatedRequestRejected,
    MissingEntityTag,
    UploadExpired,
    UploadStateConflict,
};

struct CloudTransferError {
    CloudTransferErrorCode code = CloudTransferErrorCode::InvalidResponse;
    int httpStatus = 0;
    QString apiCode;
    QString safeMessage;
    QString serverRequestId;
    bool retryable = false;
};

enum class CloudAssetKind : quint8 { Audio, Sample, PluginState, Other };

/// The uploadId is caller-created and stable across retries/restarts. The
/// optional checksum/size are strict preconditions; when omitted, the worker
/// computes them before the first prepare request.
struct CloudAssetUploadInput {
    QString projectId;
    QString uploadId;
    QString assetId;
    QString sourcePath;
    QString sha256;
    quint64 byteSize = 0;
    CloudAssetKind kind = CloudAssetKind::Audio;
    QString contentType;
    QString displayName;
};

struct CloudSnapshotUploadInput {
    QString projectId;
    QString uploadId;
    QString sourcePath;
    quint64 sequence = 0;
    int schemaVersion = 7;
    QString sha256;
    quint64 byteSize = 0;
    QString contentType = QStringLiteral("application/vnd.vlt.project+json");
    QStringList assetIds;
};

struct CloudSnapshotDownloadInput {
    QString projectId;
    QString snapshotId;
    /// Bootstrap carries only the authorized snapshot id/sequence. Empty
    /// sha256 and zero byteSize therefore mean that both values must be
    /// discovered from the authenticated preparation response. When supplied,
    /// either value remains a strict caller precondition.
    QString sha256;
    quint64 byteSize = 0;
    QString destinationPath;
};

struct CloudBlobDescriptor {
    QString id;
    QString sha256;
    quint64 byteSize = 0;
    QString contentType;
    QString kind;
    QString createdBy;
    QDateTime createdAt;
    QDateTime verifiedAt;
};

struct CloudAssetUploadResult {
    QString projectId;
    QString uploadId;
    QString blobId;
    daw::AssetRef asset;
    QString contentType;
};

struct CloudSnapshotUploadResult {
    QString projectId;
    QString uploadId;
    QString snapshotId;
    QString blobId;
    quint64 sequence = 0;
    int schemaVersion = 0;
    QString sha256;
    quint64 byteSize = 0;
    QString contentType;
    QStringList assetIds;
};

struct CloudSnapshotDownloadResult {
    QString projectId;
    QString snapshotId;
    QString sha256;
    quint64 byteSize = 0;
    QString contentType;
    QString localPath;
};

/// File and network transfer boundary for collaboration blobs.
///
/// The implementation owns a worker event loop. Hashing, QFile/QSaveFile I/O,
/// request-body streaming, verification and AssetCache registration never run
/// on the caller/UI thread (and must never be called from an audio callback).
class CloudAssetTransferManager final : public QObject {
    Q_OBJECT
public:
    explicit CloudAssetTransferManager(account::Service* account,
                                       AssetCache* cache,
                                       QObject* parent = nullptr);
    ~CloudAssetTransferManager() override;

    int requestTimeoutMs() const;
    void setRequestTimeoutMs(int timeoutMs);

    quint64 uploadAsset(const CloudAssetUploadInput& input);
    quint64 uploadSnapshot(const CloudSnapshotUploadInput& input);
    quint64 downloadAsset(const QString& projectId,
                          const daw::AssetRef& expected);
    quint64 downloadSnapshot(const CloudSnapshotDownloadInput& input);
    quint64 abortUpload(const QString& projectId, const QString& uploadId);

    /// A failed upload re-enters prepare with the same uploadId. Multipart
    /// provider state is observed again before any byte is resent.
    bool retry(quint64 transferId);
    bool cancel(quint64 transferId);
    void cancelAll();

signals:
    void transferStateChanged(quint64 transferId, CloudTransferKind kind,
                              CloudTransferState state);
    void transferProgress(quint64 transferId, CloudTransferKind kind,
                          quint64 completedBytes, quint64 totalBytes);
    void assetUploadCompleted(quint64 transferId,
                              const CloudAssetUploadResult& result);
    void snapshotUploadCompleted(quint64 transferId,
                                 const CloudSnapshotUploadResult& result);
    void assetDownloadCompleted(quint64 transferId,
                                const daw::AssetRef& asset,
                                const QString& localPath);
    void snapshotDownloadCompleted(
        quint64 transferId, const CloudSnapshotDownloadResult& result);
    void uploadAborted(quint64 transferId, const QString& projectId,
                       const QString& uploadId);
    void transferFailed(quint64 transferId, CloudTransferKind kind,
                        const CloudTransferError& error);

private:
    struct Credentials;
    using CredentialProvider = std::function<Credentials()>;
    using NetworkFactory = std::function<QNetworkAccessManager*()>;

    CloudAssetTransferManager(CredentialProvider credentialProvider,
                              NetworkFactory networkFactory,
                              AssetCache* cache, QObject* parent);
    void refreshCredentials();

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend bool checkCloudAssetTransferManagerForTest(QString* error);
};

bool checkCloudAssetTransferManagerForTest(QString* error = nullptr);

} // namespace collab

Q_DECLARE_METATYPE(collab::CloudTransferKind)
Q_DECLARE_METATYPE(collab::CloudTransferState)
Q_DECLARE_METATYPE(collab::CloudTransferError)
Q_DECLARE_METATYPE(collab::CloudAssetUploadResult)
Q_DECLARE_METATYPE(collab::CloudSnapshotUploadResult)
Q_DECLARE_METATYPE(collab::CloudSnapshotDownloadResult)
