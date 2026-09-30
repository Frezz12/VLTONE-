#pragma once

#include "collaboration/PluginCompatibility.hpp"
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

namespace collab {
class AssetCache;
class CloudProjectClient;
class CloudProjectSyncCoordinator;
class CollaborationService;
class CollaborationCommandBridge;

/// Called only by an owner's explicit Start action after isolated plugin
/// probes. Proves a current local decode before upgrading inactive metadata.
class CloudProjectVersionCoordinator final : public QObject {
    Q_OBJECT
public:
    using Completion = std::function<void(bool, const QString&)>;
    CloudProjectVersionCoordinator(CloudProjectClient* projects,
        CloudProjectSyncCoordinator* synchronizer, CollaborationService* service,
        CollaborationCommandBridge* bridge, AssetCache* cache, QObject* parent = nullptr);
    bool ensureCurrent(const QString& projectId, const QString& engineVersion,
        const QString& appVersion, const daw::collab::PluginReadinessReport& report,
        Completion completion);
    void cancel();
    bool busy() const noexcept { return bool(m_completion); }
private:
    bool proofStillCurrent() const;
    void finish(bool success, const QString& message = {});
    QPointer<CloudProjectClient> m_projects;
    QPointer<CloudProjectSyncCoordinator> m_sync;
    QPointer<CollaborationService> m_service;
    QPointer<CollaborationCommandBridge> m_bridge;
    QPointer<AssetCache> m_cache;
    Completion m_completion;
    daw::collab::PluginReadinessReport m_report;
    QString m_projectId, m_engineVersion, m_appVersion;
    quint64 m_head = 0, m_generation = 0, m_request = 0;
};

bool checkCloudProjectVersionCoordinatorForTest(QString* error = nullptr);
} // namespace collab
