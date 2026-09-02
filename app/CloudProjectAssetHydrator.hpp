#pragma once

#include "model/Document.hpp"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace collab {

class AssetCache;
class CloudAssetTransferManager;
enum class CloudTransferKind : quint8;
struct CloudTransferError;

enum class CloudHydrationState : quint8 {
    Idle,
    Downloading,
    Ready,
    Degraded,
};

/// Returns every durable blob referenced by a shared musical document. Exact
/// duplicate references are collapsed; a conflicting reuse of the same
/// assetId is deliberately retained so the hydrator can reject it visibly.
QList<daw::AssetRef> collectCloudProjectAssets(const daw::ProjectModel& project);

/// Bounded download queue for a materialized cloud project.
///
/// Snapshot/reducer installation never waits for media. Missing clips are
/// silent placeholders in the engine projection while this queue fills the
/// content-addressed AssetCache on worker threads. AssetCache::assetReady then
/// lets the projection refresh only its runtime paths.
class CloudProjectAssetHydrator final : public QObject {
    Q_OBJECT
public:
    CloudProjectAssetHydrator(CloudAssetTransferManager* transfers,
                              AssetCache* cache,
                              QObject* parent = nullptr);

    /// Replaces the desired generation. Existing transfers are cancelled;
    /// verified cache hits make a repeated bootstrap cheap and prevent stale
    /// failures/progress from leaking into the new project head.
    void hydrate(const QString& projectId,
                 const QList<daw::AssetRef>& assets,
                 const QStringList& priorityAssetIds = {});
    /// Adds newly referenced assets to the current project's queue without
    /// cancelling downloads started by the bootstrap generation.
    void ensureMissing(const QString& projectId,
                       const QList<daw::AssetRef>& assets,
                       const QStringList& priorityAssetIds = {});
    /// Retries the retryable failures from the current project generation.
    /// These items keep their original progress slots, so total never grows.
    void retryFailed();
    void cancel();

    CloudHydrationState state() const noexcept { return m_state; }
    QString projectId() const { return m_projectId; }
    qsizetype pendingCount() const noexcept;
    qsizetype failedCount() const noexcept { return m_failedCount; }

signals:
    void stateChanged(collab::CloudHydrationState state);
    void progressChanged(qsizetype completed, qsizetype total);
    void assetUnavailable(const QString& assetId,
                          const QString& safeMessage,
                          bool retryable);
    void hydrationSettled(const QString& projectId,
                          qsizetype failedCount);

private:
    struct Item {
        daw::AssetRef asset;
        int attempts = 0;
        quint64 generation = 0;
    };

    void setState(CloudHydrationState state);
    void appendAssets(const QList<daw::AssetRef>& assets,
                      const QStringList& priorityAssetIds);
    void pump();
    void finishIfSettled();
    void handleCompleted(quint64 transferId,
                         const daw::AssetRef& asset,
                         const QString& localPath);
    void handleFailed(quint64 transferId,
                      CloudTransferKind kind,
                      const CloudTransferError& error);
    static QString normalizedProjectId(const QString& value);
    static QString normalizedAssetId(const daw::AssetRef& asset);
    static QString normalizedHash(const daw::AssetRef& asset);
    static bool completeAsset(const daw::AssetRef& asset);

    CloudAssetTransferManager* m_transfers = nullptr;
    AssetCache* m_cache = nullptr;
    QString m_projectId;
    QList<Item> m_queue;
    QHash<quint64, Item> m_active;
    QSet<QString> m_activeHashes;
    QHash<QString, QString> m_knownIdentities;
    QHash<QString, Item> m_failed;
    quint64 m_generation = 0;
    qsizetype m_totalCount = 0;
    qsizetype m_completedCount = 0;
    qsizetype m_failedCount = 0;
    qsizetype m_scheduledRetryCount = 0;
    CloudHydrationState m_state = CloudHydrationState::Idle;

    static constexpr qsizetype kMaximumAssetsPerProject = 100000;
    static constexpr qsizetype kMaximumConcurrentDownloads = 3;
    static constexpr int kMaximumAttempts = 3;
};

bool checkCloudProjectAssetHydratorForTest(QString* error = nullptr);

} // namespace collab

Q_DECLARE_METATYPE(collab::CloudHydrationState)
