#pragma once

#include "CollaborationTypes.hpp"

#include <QObject>
#include <QString>

#include <memory>

namespace collab {

class CloudAssetTransferManager;
class CollaborationCommandBridge;
class CollaborationService;

/// Converts server-assigned snapshot.requested controls into exact canonical
/// snapshot uploads. Only one request performs serialization/network I/O at a
/// time; repeated delivery is bounded and idempotent by requestId/attempt.
class SnapshotRequestUploader final : public QObject {
    Q_OBJECT
public:
    SnapshotRequestUploader(CollaborationService* service,
                            CollaborationCommandBridge* bridge,
                            CloudAssetTransferManager* transfers,
                            QObject* parent = nullptr);
    ~SnapshotRequestUploader() override;

    void cancel();

signals:
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

    friend bool checkSnapshotRequestUploaderForTest(QString* error);
};

bool checkSnapshotRequestUploaderForTest(QString* error = nullptr);

} // namespace collab

