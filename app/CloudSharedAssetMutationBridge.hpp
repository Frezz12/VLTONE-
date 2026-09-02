#pragma once

#include "collaboration/SharedAssetMutationSink.hpp"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

namespace daw { class EngineController; }

namespace collab {

class CollaborationService;
class CloudSharedAssetMutationCoordinator;
struct CloudSharedAssetMutationResult;

/// Binds EngineController's local-only asset requests to the verified upload
/// coordinator. Exactly one request is active so retry/cancel and project
/// generation ownership stay unambiguous.
class CloudSharedAssetMutationBridge final
    : public QObject,
      public daw::collab::SharedAssetMutationSink {
    Q_OBJECT
public:
    CloudSharedAssetMutationBridge(
        CloudSharedAssetMutationCoordinator* coordinator,
        CollaborationService* service, daw::EngineController* controller,
        QObject* parent = nullptr);
    ~CloudSharedAssetMutationBridge() override;

    daw::collab::SharedMutationResult prepare(
        daw::collab::SharedAssetMutationRequest request) override;

    bool active() const noexcept { return !m_requestId.isEmpty(); }
    QString requestId() const { return m_requestId; }
    bool retry();
    void cancel();

signals:
    void activeChanged(bool active);
    void progressChanged(qsizetype completedFiles, qsizetype totalFiles);
    void failed(const QString& safeMessage, bool retryable);
    void completed(bool commandSubmitted);
    void cancelled();

private:
    void handleBatchReady(
        quint64 generation,
        const QVector<collab::CloudSharedAssetMutationResult>& results);
    void handleBatchFailed(quint64 generation, const QString& safeMessage,
                           bool retryable);
    void handleBatchCancelled(quint64 generation);
    void invalidateActive(bool notify);
    QString currentProjectId() const;

    QPointer<CloudSharedAssetMutationCoordinator> m_coordinator;
    QPointer<CollaborationService> m_service;
    daw::EngineController* m_controller = nullptr;
    QString m_projectId;
    QString m_requestId;
    quint64 m_generation = 0;
};

} // namespace collab
