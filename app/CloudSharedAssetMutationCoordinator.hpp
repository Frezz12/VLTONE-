#pragma once

#include "CloudRecordingAssetCoordinator.hpp"

#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

namespace collab {

/// One local audio source that must become an immutable cloud AssetRef before
/// its caller is allowed to build or submit a shared command. The two UUIDs
/// are caller-created and stay stable across retries. sourcePath is local-only.
struct CloudSharedAssetMutationInput {
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

struct CloudSharedAssetMutationResult {
    QString projectId;
    QString uploadId;
    /// Runtime/cache edge data. Never copy this value into a command.
    QString cachedLocalPath;
    /// Complete only after backend upload verification has succeeded.
    daw::AssetRef asset;
};

enum class CloudSharedAssetMutationState : quint8 {
    Idle,
    Importing,
    Uploading,
    Ready,
    Failed,
    Cancelled,
};

/// Reusable pre-command gate for cloud audio imports and Sampler samples.
///
/// begin() performs cache import/hash first and verified upload second. This
/// class deliberately has no mutation sink: batchReady is the sole point at
/// which a caller may construct one durable command/batch from the AssetRefs.
class CloudSharedAssetMutationCoordinator final : public QObject {
    Q_OBJECT
public:
    CloudSharedAssetMutationCoordinator(CloudAssetTransferManager* transfers,
                                        AssetCache* cache,
                                        QObject* parent = nullptr);
    ~CloudSharedAssetMutationCoordinator() override;

    /// Replaces active work. V1 bounds one atomic set to 1..8 files.
    quint64 begin(const QVector<CloudSharedAssetMutationInput>& inputs);
    bool retry(const QString& uploadId);
    void retryFailed();
    void cancel();

    quint64 generation() const noexcept;
    CloudSharedAssetMutationState state() const noexcept;

signals:
    void stateChanged(quint64 generation,
                      collab::CloudSharedAssetMutationState state);
    void progressChanged(quint64 generation, qsizetype completedFiles,
                         qsizetype totalFiles);
    void assetReady(quint64 generation,
                    const collab::CloudSharedAssetMutationResult& result);
    void batchReady(
        quint64 generation,
        const QVector<collab::CloudSharedAssetMutationResult>& results);
    void itemFailed(quint64 generation, const QString& uploadId,
                    const QString& safeMessage, bool retryable);
    void batchFailed(quint64 generation, const QString& safeMessage,
                     bool retryable);
    void batchCancelled(quint64 generation);

private:
    explicit CloudSharedAssetMutationCoordinator(
        CloudRecordingAssetPort* port, QObject* parent = nullptr);

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend bool checkCloudSharedAssetMutationCoordinatorForTest(
        QString* error);
};

bool checkCloudSharedAssetMutationCoordinatorForTest(QString* error = nullptr);

} // namespace collab

Q_DECLARE_METATYPE(collab::CloudSharedAssetMutationState)
Q_DECLARE_METATYPE(collab::CloudSharedAssetMutationResult)
