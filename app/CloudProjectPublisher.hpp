#pragma once

#include "CloudAssetTransferManager.hpp"
#include "CloudProjectClient.hpp"
#include "collaboration/CollaborationState.hpp"

#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

namespace collab {

/// A local byte source for one immutable AssetRef already present in the
/// staged ProjectModel. The source path is never copied into the cloud
/// document or sent as metadata. UUID/kind/declared size come from the staged
/// AssetRef; sha256 may still be empty and is then filled by the transfer
/// worker after it hashes and verifies the local bytes.
struct CloudPublicationAssetSource {
    QString assetId;
    QString sourcePath;
    QString contentType;
};

struct CloudProjectPublicationInput {
    daw::ProjectModel project;
    CreateCloudProjectInput metadata;
    QVector<CloudPublicationAssetSource> assetSources;
};

enum class CloudPublicationPhase : quint8 {
    Idle,
    Preflight,
    CreatingProject,
    UploadingAssets,
    PreparingSnapshot,
    UploadingSnapshot,
    Activating,
    Completed,
    Failed,
    Cancelled,
};

struct CloudProjectPublicationResult {
    CloudProjectView project;
    CloudSnapshotUploadResult snapshot;
    /// Exact canonical seq-0 state that was uploaded. This is a detached copy;
    /// the caller's local ProjectModel is never mutated by publication.
    daw::collab::SharedProjectDocument canonicalDocument;
};

/// End-to-end initial publication coordinator for an already staged local
/// project. It deliberately does not capture plugin state or discover local
/// media paths: the caller supplies an immutable ProjectModel plus an exact
/// assetId -> local source mapping after those operations have completed.
///
/// Model inspection, canonical serialization, snapshot hashing and staging
/// file I/O run on a worker. Asset hashing/file/network work remains owned by
/// CloudAssetTransferManager. The publisher only advances its state machine on
/// its QObject thread and isolates every callback by publication generation.
class CloudProjectPublisher final : public QObject {
    Q_OBJECT
public:
    CloudProjectPublisher(CloudProjectClient* projects,
                          CloudAssetTransferManager* transfers,
                          QObject* parent = nullptr);
    ~CloudProjectPublisher() override;

    /// Replaces any active publication and returns its non-zero generation.
    quint64 publish(const CloudProjectPublicationInput& input);
    void cancel();

    CloudPublicationPhase phase() const noexcept;
    quint64 generation() const noexcept;
    QString cloudProjectId() const;

    /// Asset transfers are bounded independently of the transfer manager.
    /// Values outside [1, 4] are clamped; the default is 3.
    void setMaximumConcurrentAssetUploads(int maximum);
    int maximumConcurrentAssetUploads() const noexcept;

signals:
    void phaseChanged(quint64 generation,
                      collab::CloudPublicationPhase phase);
    /// Deterministic monotonic unit progress. The total is assets + create +
    /// snapshot + activation; byte-level transfer ordering is intentionally
    /// not exposed as publication progress.
    void progressChanged(quint64 generation, int completedUnits,
                         int totalUnits);
    void publicationFailed(quint64 generation,
                           collab::CloudPublicationPhase failedPhase,
                           const QString& cloudProjectId,
                           const QString& safeMessage, bool retryable);
    void publicationCancelled(quint64 generation,
                              const QString& cloudProjectId);
    void publicationCompleted(
        quint64 generation,
        const collab::CloudProjectPublicationResult& result);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    friend bool checkCloudProjectPublisherForTest(QString* error);
};

bool checkCloudProjectPublisherForTest(QString* error = nullptr);

} // namespace collab

Q_DECLARE_METATYPE(collab::CloudPublicationPhase)
Q_DECLARE_METATYPE(collab::CloudProjectPublicationResult)
