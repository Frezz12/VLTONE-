#pragma once

#include "CloudSharedAssetMutationCoordinator.hpp"
#include "collaboration/SharedAssetMutationSink.hpp"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

namespace daw { class EngineController; }

namespace collab {

class CollaborationService;

/// Binds EngineController's local-only asset requests to the verified upload
/// coordinator. One request is uploading at a time; further requests queue
/// behind it so a multi-file drop imports every file. Retry/cancel and project
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
    /// Imports accepted but not yet started. Dropping several files at once
    /// must import all of them, not just the first.
    qsizetype queuedCount() const noexcept { return m_queued.size(); }
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
    /// Hands the coordinator the next queued import. The coordinator owns one
    /// generation at a time, so requests are started in order rather than
    /// refused while one is in flight.
    bool startNextQueued();
    void discardQueued();

    QPointer<CloudSharedAssetMutationCoordinator> m_coordinator;
    QPointer<CollaborationService> m_service;
    daw::EngineController* m_controller = nullptr;
    QString m_projectId;
    QString m_requestId;
    quint64 m_generation = 0;
    QList<CloudSharedAssetMutationInput> m_queued;
};

} // namespace collab
