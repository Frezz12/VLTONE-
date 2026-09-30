#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

namespace collab {
class CloudProjectClient;
class CloudProjectSyncCoordinator;
class CollaborationService;
class CollaborationCommandBridge;

/// Reconciles an owner's inactive cloud-list rename with the verified document
/// through an ordinary conditional command after its room becomes writable.
class CloudProjectTitleCoordinator final : public QObject {
    Q_OBJECT
public:
    CloudProjectTitleCoordinator(CloudProjectClient* projects,
                                 CloudProjectSyncCoordinator* synchronizer,
                                 CollaborationService* service,
                                 CollaborationCommandBridge* bridge,
                                 QObject* parent = nullptr);
    bool pending() const noexcept { return m_pending; }
signals:
    void pendingChanged(bool pending);
    void failed(const QString& safeMessage);
private:
    void pump();
    void clear();
    void setPending(bool pending);
    QPointer<CloudProjectSyncCoordinator> m_sync;
    QPointer<CollaborationService> m_service;
    QPointer<CollaborationCommandBridge> m_bridge;
    QString m_projectId;
    QString m_title;
    QString m_expectedWriter;
    QString m_operationId;
    quint64 m_generation = 0;
    quint64 m_bootstrapHead = 0;
    bool m_verified = false;
    bool m_pending = false;
};

bool checkCloudProjectTitleCoordinatorForTest(QString* error = nullptr);
} // namespace collab
