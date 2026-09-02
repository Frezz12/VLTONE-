#include "CloudProjectSyncCoordinator.hpp"

#include "CloudAssetTransferManager.hpp"
#include "CloudProjectAssetHydrator.hpp"
#include "CloudProjectCache.hpp"
#include "CloudProjectPublisher.hpp"
#include "CloudSnapshotAssetManifest.hpp"
#include "CollaborationCommandBridge.hpp"
#include "CollaborationService.hpp"
#include "ProjectSerializer.hpp"
#include "SnapshotRequestUploader.hpp"
#include "collaboration/CommandJson.hpp"
#include "collaboration/ProjectReducer.hpp"
#include "collaboration/SharedProjectSnapshot.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>

namespace collab {
namespace {

using daw::collab::ProjectReducer;
using daw::collab::SharedProjectDocument;

struct Materialization {
    bool accepted = false;
    CloudSyncError error;
    SharedProjectDocument document;
    QString canonicalHash;
};

QString canonicalUuid(const QString& value) {
    const QUuid uuid(value);
    return uuid.isNull()
        ? QString()
        : uuid.toString(QUuid::WithoutBraces).toLower();
}

bool provesNoActiveSession(const CloudClientError& error) {
    return error.httpStatus == 404 &&
           error.apiCode == QLatin1String("collaboration_not_found");
}

enum class JournalLookupDecision : quint8 {
    Retire,
    Resubmit,
    Resync,
};

bool sameRecoveredCommand(
    const daw::collab::ProjectCommand& journaled,
    const CloudProjectOperation& committed) {
    const QString projectId = canonicalUuid(committed.projectId);
    if (projectId.isEmpty() ||
        canonicalUuid(QString::fromStdString(journaled.meta.projectId)) !=
            projectId ||
        canonicalUuid(QString::fromStdString(
            committed.command.meta.projectId)) != projectId ||
        committed.serverSequence == 0 ||
        committed.command.meta.serverSequence != committed.serverSequence ||
        daw::collab::serializeProjectCommand(journaled) !=
            daw::collab::serializeProjectCommand(committed.command)) {
        return false;
    }

    const auto identityMatches = [](const std::string& journaledId,
                                    const QString& committedId,
                                    const std::string& commandId) {
        const QString canonicalCommitted = canonicalUuid(committedId);
        if (canonicalCommitted.isEmpty() ||
            canonicalUuid(QString::fromStdString(commandId)) !=
                canonicalCommitted) {
            return false;
        }
        return journaledId.empty() ||
               canonicalUuid(QString::fromStdString(journaledId)) ==
                   canonicalCommitted;
    };
    return identityMatches(journaled.meta.actorId, committed.actorUserId,
                           committed.command.meta.actorId) &&
           identityMatches(journaled.meta.clientId, committed.actorDeviceId,
                           committed.command.meta.clientId);
}

JournalLookupDecision decideJournalLookup(
    const QString& expectedProjectId, quint64 expectedHead,
    const daw::collab::ProjectCommand& journaled,
    const CloudOperationLookup& lookup) {
    const QString operationId =
        QString::fromStdString(journaled.meta.operationId);
    if (lookup.projectId != expectedProjectId ||
        lookup.operationId != operationId ||
        lookup.headSequence != expectedHead) {
        return JournalLookupDecision::Resync;
    }
    if (!lookup.operation) return JournalLookupDecision::Resubmit;
    if (lookup.operation->serverSequence > lookup.headSequence ||
        !sameRecoveredCommand(journaled, *lookup.operation)) {
        return JournalLookupDecision::Resync;
    }
    return JournalLookupDecision::Retire;
}

Materialization rejected(CloudSyncErrorCode code, const QString& message) {
    Materialization result;
    result.error.code = code;
    result.error.safeMessage = message;
    return result;
}

Materialization materialize(const CloudProjectBootstrap& bootstrap,
                            const QString& snapshotPath) {
    const QString projectId = canonicalUuid(bootstrap.project.id);
    if (projectId.isEmpty() || bootstrap.project.formatVersion !=
                                   daw::ProjectSerializer::kFormatVersion ||
        bootstrap.headSequence != bootstrap.project.headSequence ||
        bootstrap.replayBaseSequence > bootstrap.headSequence) {
        return rejected(CloudSyncErrorCode::SnapshotInvalid,
                        QStringLiteral("Cloud bootstrap metadata is invalid"));
    }

    SharedProjectDocument document;
    quint64 snapshotSequence = 0;
    if (bootstrap.snapshot) {
        if (bootstrap.snapshot->projectId != projectId ||
            bootstrap.snapshot->schemaVersion !=
                daw::ProjectSerializer::kFormatVersion ||
            bootstrap.snapshot->sequence != bootstrap.replayBaseSequence ||
            !isCanonicalCloudSnapshotAssetManifest(
                bootstrap.snapshot->assetIds) ||
            snapshotPath.isEmpty()) {
            return rejected(CloudSyncErrorCode::SnapshotInvalid,
                            QStringLiteral("Cloud snapshot metadata is invalid"));
        }
        QFile file(snapshotPath);
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
            quint64(file.size()) >
                daw::collab::kMaximumSharedProjectSnapshotBytes) {
            return rejected(CloudSyncErrorCode::SnapshotInvalid,
                            QStringLiteral("Cloud snapshot bytes are unavailable"));
        }
        const QByteArray bytes = file.readAll();
        if (bytes.size() != file.size()) {
            return rejected(CloudSyncErrorCode::SnapshotInvalid,
                            QStringLiteral("Cloud snapshot could not be read"));
        }
        const audio::Result decoded =
            daw::collab::deserializeSharedProjectSnapshot(
                document,
                std::string_view(bytes.constData(), std::size_t(bytes.size())));
        if (!decoded || document.confirmedSequence !=
                            bootstrap.snapshot->sequence) {
            return rejected(CloudSyncErrorCode::SnapshotInvalid,
                            QStringLiteral("Cloud snapshot failed validation"));
        }
        const CloudSnapshotAssetManifest manifest =
            collectCloudSnapshotAssetManifest(document.project);
        if (!manifest.accepted ||
            manifest.assetIds != bootstrap.snapshot->assetIds) {
            return rejected(
                CloudSyncErrorCode::SnapshotInvalid,
                QStringLiteral("Cloud snapshot asset manifest mismatched"));
        }
        snapshotSequence = bootstrap.snapshot->sequence;
    } else if (bootstrap.replayBaseSequence != 0) {
        return rejected(CloudSyncErrorCode::SnapshotInvalid,
                        QStringLiteral("Cloud bootstrap omitted its base snapshot"));
    }

    std::unordered_map<std::string, quint64> replaySequenceByOperation;
    quint64 expectedSequence = snapshotSequence + 1;
    for (const CloudProjectOperation& operation : bootstrap.operations) {
        const daw::collab::ProjectCommand& command = operation.command;
        if (operation.projectId != projectId ||
            operation.serverSequence != expectedSequence ||
            command.meta.serverSequence != expectedSequence ||
            command.meta.projectId != projectId.toStdString()) {
            return rejected(CloudSyncErrorCode::ReplayRejected,
                            QStringLiteral("Cloud operation sequence is invalid"));
        }
        const daw::collab::ApplyResult applied =
            ProjectReducer::apply(document, command);
        if (!applied.accepted() ||
            applied.code == daw::collab::ApplyCode::Duplicate) {
            return rejected(CloudSyncErrorCode::ReplayRejected,
                            QStringLiteral("Cloud operation replay was rejected"));
        }
        document.confirmedSequence = expectedSequence;
        replaySequenceByOperation.emplace(command.meta.operationId,
                                          expectedSequence);
        ++expectedSequence;
    }
    if (document.confirmedSequence != bootstrap.headSequence ||
        expectedSequence != bootstrap.headSequence + 1) {
        return rejected(CloudSyncErrorCode::ReplayRejected,
                        QStringLiteral("Cloud operation log has a sequence gap"));
    }

    if (bootstrap.fieldHeads.size() !=
        qsizetype(document.lastWriterByField.size())) {
        return rejected(CloudSyncErrorCode::FieldHeadsMismatch,
                        QStringLiteral("Cloud field heads do not match the project"));
    }
    for (const CloudProjectFieldHead& head : bootstrap.fieldHeads) {
        const auto local = document.lastWriterByField.find(
            head.fieldKey.toStdString());
        if (head.projectId != projectId ||
            local == document.lastWriterByField.end() ||
            local->second != head.operationId.toStdString() ||
            head.headSequence > bootstrap.headSequence) {
            return rejected(CloudSyncErrorCode::FieldHeadsMismatch,
                            QStringLiteral("Cloud field heads do not match the project"));
        }
        const auto replayed = replaySequenceByOperation.find(local->second);
        if ((replayed != replaySequenceByOperation.end() &&
             replayed->second != head.headSequence) ||
            (replayed == replaySequenceByOperation.end() &&
             head.headSequence > snapshotSequence)) {
            return rejected(CloudSyncErrorCode::FieldHeadsMismatch,
                            QStringLiteral("Cloud field-head sequence is invalid"));
        }
    }

    std::string canonicalBytes;
    const audio::Result encoded =
        daw::collab::serializeSharedProjectSnapshot(document, canonicalBytes);
    if (!encoded) {
        return rejected(CloudSyncErrorCode::SnapshotInvalid,
                        QStringLiteral("Canonical project state is invalid"));
    }
    const QByteArray digest = QCryptographicHash::hash(
        QByteArray(canonicalBytes.data(), qsizetype(canonicalBytes.size())),
        QCryptographicHash::Sha256).toHex();

    Materialization result;
    result.accepted = true;
    result.document = std::move(document);
    result.canonicalHash = QString::fromLatin1(digest);
    return result;
}

} // namespace

struct CloudProjectSyncCoordinator::Impl {
    CloudProjectSyncCoordinator* q = nullptr;
    QPointer<CloudProjectClient> projects;
    QPointer<CloudAssetTransferManager> transfers;
    QPointer<CollaborationCommandBridge> bridge;
    QPointer<CollaborationService> service;
    QString snapshotDirectory;
    QString projectCacheRoot;
    CloudSyncPhase phase = CloudSyncPhase::Idle;
    QString projectId;
    quint64 generation = 0;
    quint64 bootstrapRequest = 0;
    quint64 snapshotTransfer = 0;
    quint64 sessionRequest = 0;
    quint64 journalLookupRequest = 0;
    quint64 hashTaskGeneration = 0;
    bool connectIfLive = true;
    bool resumeConnectedRoom = false;
    std::shared_ptr<CloudProjectBootstrap> bootstrap;
    std::unique_ptr<SnapshotRequestUploader> snapshotRequests;
    QVector<daw::collab::ProjectCommand> journalLookups;
    QVector<daw::collab::ProjectCommand> recoveryCommands;
    QSet<QString> recoveryOutstanding;
    QSet<QString> recoverySubmitted;
    qsizetype journalLookupIndex = 0;
    QString installedCanonicalHash;
    QList<daw::AssetRef> installedAssets;
    CloudProjectRole installedRole = CloudProjectRole::Viewer;
    CloudProjectStatus installedStatus = CloudProjectStatus::Archived;
    CloudProjectStatus bootstrapStatus = CloudProjectStatus::Archived;
    bool installedOffline = false;

    explicit Impl(CloudProjectSyncCoordinator* owner) : q(owner) {
        snapshotDirectory = QDir(QStandardPaths::writableLocation(
                                     QStandardPaths::CacheLocation))
                                .filePath(QStringLiteral(
                                    "collaboration/snapshot-staging-v2"));
        if (!QDir().mkpath(snapshotDirectory)) snapshotDirectory.clear();
        projectCacheRoot = CloudProjectCache::defaultRootDirectory();
    }

    bool active() const {
        return phase == CloudSyncPhase::FetchingBootstrap ||
               phase == CloudSyncPhase::DownloadingSnapshot ||
               phase == CloudSyncPhase::ReplayingOperations ||
               phase == CloudSyncPhase::ReconcilingPending ||
               phase == CloudSyncPhase::CheckingLiveSession;
    }

    void setPhase(CloudSyncPhase value) {
        if (phase == value) return;
        phase = value;
        emit q->phaseChanged(phase);
    }

    void cancelRequests() {
        ++hashTaskGeneration;
        if (projects && bootstrapRequest != 0)
            projects->cancel(bootstrapRequest);
        if (projects && sessionRequest != 0)
            projects->cancel(sessionRequest);
        if (projects && journalLookupRequest != 0)
            projects->cancel(journalLookupRequest);
        if (transfers && snapshotTransfer != 0)
            transfers->cancel(snapshotTransfer);
        bootstrapRequest = 0;
        snapshotTransfer = 0;
        sessionRequest = 0;
        journalLookupRequest = 0;
        bootstrap.reset();
        journalLookups.clear();
        journalLookupIndex = 0;
        recoveryCommands.clear();
        recoveryOutstanding.clear();
        recoverySubmitted.clear();
    }

    void computeRequestedHash(const QString& roundId,
                              const QString& sessionId,
                              quint64 serverSequence,
                              qint64 deadlineMs) {
        if (!bridge || !service || roundId.isEmpty() || sessionId.isEmpty() ||
            service->projectId() != projectId ||
            service->sessionId() != sessionId || deadlineMs <= 0) return;
        auto snapshot = bridge->confirmedSnapshotAt(serverSequence);
        if (!snapshot) {
            bridge->requireResync(QStringLiteral(
                "Hash verification requires an exact confirmed project"));
            return;
        }
        const quint64 task = ++hashTaskGeneration;
        const QString taskProject = projectId;
        QPointer<CloudProjectSyncCoordinator> guard(q);
        QThreadPool::globalInstance()->start(
            [guard, task, taskProject, sessionId, serverSequence,
             snapshot = std::move(*snapshot)]() mutable {
                QString digest;
                std::string bytes;
                if (daw::collab::serializeSharedProjectSnapshot(snapshot,
                                                                 bytes)) {
                    digest = QString::fromLatin1(QCryptographicHash::hash(
                        QByteArray(bytes.data(), qsizetype(bytes.size())),
                        QCryptographicHash::Sha256).toHex());
                }
                QCoreApplication* application = QCoreApplication::instance();
                if (!application) return;
                QMetaObject::invokeMethod(
                    application,
                    [guard, task, taskProject, sessionId, serverSequence,
                     digest] {
                        if (!guard ||
                            guard->m_impl->hashTaskGeneration != task ||
                            !guard->m_impl->service ||
                            guard->m_impl->service->projectId() != taskProject ||
                            guard->m_impl->service->sessionId() != sessionId ||
                            guard->m_impl->service->bootstrapServerSequence() !=
                                serverSequence) {
                            return;
                        }
                        if (digest.isEmpty() ||
                            !guard->m_impl->service
                                 ->installVerifiedBootstrapState(
                                     taskProject, serverSequence, digest) ||
                            !guard->m_impl->service->sendSnapshotHash()) {
                            guard->m_impl->bridge->requireResync(QStringLiteral(
                                "Project hash could not be verified"));
                        }
                    },
                    Qt::QueuedConnection);
            });
    }

    void fail(CloudSyncError error) {
        cancelRequests();
        setPhase(CloudSyncPhase::Failed);
        emit q->synchronizationFailed(generation, error);
    }

    quint64 begin(const QString& requestedProject, bool shouldConnect,
                  bool fromResync = false) {
        const QString normalized = canonicalUuid(requestedProject);
        if (normalized.isEmpty() || !projects || !transfers || !bridge ||
            !service || snapshotDirectory.isEmpty()) {
            ++generation;
            fail({CloudSyncErrorCode::InvalidInput,
                  QStringLiteral("Cloud synchronization is unavailable"),
                  false});
            return generation;
        }
        if (active() && projectId == normalized && !fromResync)
            return generation;

        if (service->projectId() != normalized &&
            bridge->pendingOperationCount() != 0) {
            ++generation;
            fail({CloudSyncErrorCode::InstallRejected,
                  QStringLiteral(
                      "Wait for pending cloud edits before opening another project"),
                  true});
            return generation;
        }

        cancelRequests();
        ++generation;
        projectId = normalized;
        connectIfLive = shouldConnect;
        resumeConnectedRoom = fromResync;
        setPhase(CloudSyncPhase::FetchingBootstrap);
        bootstrapRequest = projects->bootstrapProject(projectId, 0, 500);
        return generation;
    }

    void bootstrapReady(quint64 requestId,
                        const CloudProjectBootstrap& value) {
        if (requestId != bootstrapRequest || value.project.id != projectId)
            return;
        bootstrapRequest = 0;
        bootstrapRole = value.role;
        bootstrapStatus = value.project.status;
        bootstrap = std::make_shared<CloudProjectBootstrap>(value);
        if (!value.snapshot) {
            launchMaterialization({});
            return;
        }
        setPhase(CloudSyncPhase::DownloadingSnapshot);
        CloudSnapshotDownloadInput input;
        input.projectId = projectId;
        input.snapshotId = value.snapshot->id;
        // Integrity metadata is discovered from the authenticated download
        // preparation and then enforced for the uncredentialed object GET.
        input.destinationPath = QDir(snapshotDirectory).filePath(
            value.snapshot->id + QStringLiteral(".json"));
        snapshotTransfer = transfers->downloadSnapshot(input);
    }

    void snapshotReady(quint64 transferId,
                       const CloudSnapshotDownloadResult& result) {
        if (transferId != snapshotTransfer || !bootstrap ||
            result.projectId != projectId || !bootstrap->snapshot ||
            result.snapshotId != bootstrap->snapshot->id) {
            return;
        }
        snapshotTransfer = 0;
        launchMaterialization(result.localPath);
    }

    void launchMaterialization(const QString& snapshotPath) {
        if (!bootstrap) {
            fail({CloudSyncErrorCode::BootstrapFailed,
                  QStringLiteral("Cloud bootstrap was lost"), true});
            return;
        }
        setPhase(CloudSyncPhase::ReplayingOperations);
        const quint64 taskGeneration = generation;
        const auto taskBootstrap = bootstrap;
        const QString cacheRoot = projectCacheRoot;
        QPointer<CloudProjectSyncCoordinator> guard(q);
        QThreadPool::globalInstance()->start(
            [guard, taskGeneration, taskBootstrap, snapshotPath, cacheRoot] {
                auto result = std::make_shared<Materialization>(
                    materialize(*taskBootstrap, snapshotPath));
                if (!snapshotPath.isEmpty()) QFile::remove(snapshotPath);
                if (result->accepted) {
                    QString ignored;
                    CloudProjectCache(cacheRoot).store(
                        taskBootstrap->project.id, result->document,
                        result->canonicalHash, &ignored);
                }
                QCoreApplication* application = QCoreApplication::instance();
                if (!application) return;
                QMetaObject::invokeMethod(
                    application,
                    [guard, taskGeneration, result] {
                        if (guard)
                            guard->m_impl->install(taskGeneration,
                                                   std::move(*result), false);
                    },
                    Qt::QueuedConnection);
            });
    }

    void launchOfflineCache() {
        setPhase(CloudSyncPhase::ReplayingOperations);
        const quint64 taskGeneration = generation;
        const QString taskProject = projectId;
        const QString cacheRoot = projectCacheRoot;
        QPointer<CloudProjectSyncCoordinator> guard(q);
        QThreadPool::globalInstance()->start(
            [guard, taskGeneration, taskProject, cacheRoot] {
                auto result = std::make_shared<Materialization>();
                QString cacheError;
                auto cached = CloudProjectCache(cacheRoot).load(
                    taskProject, &cacheError);
                if (!cached) {
                    *result = rejected(
                        CloudSyncErrorCode::BootstrapFailed,
                        cacheError.isEmpty()
                            ? QStringLiteral(
                                  "No verified offline project is cached")
                            : cacheError);
                } else {
                    result->accepted = true;
                    result->document = std::move(cached->document);
                    result->canonicalHash = cached->canonicalHash;
                }
                QCoreApplication* application = QCoreApplication::instance();
                if (!application) return;
                QMetaObject::invokeMethod(
                    application,
                    [guard, taskGeneration, result] {
                        if (guard)
                            guard->m_impl->install(taskGeneration,
                                                   std::move(*result), true);
                    },
                    Qt::QueuedConnection);
            });
    }

    void finishInstalledProject() {
        emit q->synchronizedProject(
            projectId, service->bootstrapServerSequence(),
            installedCanonicalHash, installedRole, installedStatus);
        if (!installedOffline)
            emit q->projectAssetsDiscovered(projectId, installedAssets);

        if (installedOffline) {
            service->trustedOfflineProjectOpened();
            setPhase(CloudSyncPhase::Ready);
            emit q->noActiveSession(projectId);
            return;
        }

        if (resumeConnectedRoom) {
            setPhase(CloudSyncPhase::Ready);
            service->sendSnapshotHash();
            attemptRecoverySubmit();
            return;
        }
        if (!connectIfLive) {
            setPhase(CloudSyncPhase::Ready);
            return;
        }
        setPhase(CloudSyncPhase::CheckingLiveSession);
        sessionRequest = projects->getActiveSession(projectId);
    }

    void finishJournalLookups() {
        journalLookupRequest = 0;
        journalLookups.clear();
        journalLookupIndex = 0;
        recoveryOutstanding.clear();
        recoverySubmitted.clear();
        for (const daw::collab::ProjectCommand& command : recoveryCommands) {
            recoveryOutstanding.insert(
                QString::fromStdString(command.meta.operationId));
        }
        service->setPendingRecoveryBlocked(
            !recoveryOutstanding.isEmpty());
        finishInstalledProject();
    }

    void lookupNextJournalOperation() {
        if (journalLookupIndex >= journalLookups.size()) {
            finishJournalLookups();
            return;
        }
        const daw::collab::ProjectCommand& command =
            journalLookups.at(journalLookupIndex);
        journalLookupRequest = projects->lookupOperation(
            projectId, QString::fromStdString(command.meta.operationId));
    }

    void startJournalReconciliation() {
        recoveryCommands.clear();
        journalLookupIndex = 0;
        if (journalLookups.isEmpty()) {
            service->setPendingRecoveryBlocked(false);
            finishInstalledProject();
            return;
        }
        service->setPendingRecoveryBlocked(true);
        setPhase(CloudSyncPhase::ReconcilingPending);
        lookupNextJournalOperation();
    }

    void journalLookupReady(quint64 requestId,
                            const CloudOperationLookup& lookup) {
        if (requestId != journalLookupRequest ||
            journalLookupIndex >= journalLookups.size()) return;
        const daw::collab::ProjectCommand& command =
            journalLookups.at(journalLookupIndex);
        const QString operationId =
            QString::fromStdString(command.meta.operationId);
        journalLookupRequest = 0;
        const JournalLookupDecision decision = decideJournalLookup(
            projectId, service->bootstrapServerSequence(), command, lookup);
        if (decision == JournalLookupDecision::Resync) {
            bridge->requireResync(QStringLiteral(
                "Recovered operation conflicts with the server log"));
            return;
        }
        if (decision == JournalLookupDecision::Retire) {
            if (!bridge->retireJournaledOperation(projectId, operationId)) {
                fail({CloudSyncErrorCode::InstallRejected,
                      QStringLiteral(
                          "Confirmed pending operation could not be retired"),
                      true});
                return;
            }
        } else {
            recoveryCommands.push_back(command);
        }
        ++journalLookupIndex;
        lookupNextJournalOperation();
    }

    void journalLookupFailed(quint64 requestId,
                             const CloudClientError& error) {
        if (requestId != journalLookupRequest) return;
        journalLookupRequest = 0;
        fail({CloudSyncErrorCode::InstallRejected,
              QStringLiteral("Pending cloud edits could not be verified"),
              error.retryable});
    }

    void attemptRecoverySubmit() {
        if (!service || !bridge || recoveryOutstanding.isEmpty() ||
            !service->canSubmitRecoveryOperations()) return;
        for (const daw::collab::ProjectCommand& command : recoveryCommands) {
            const QString operationId =
                QString::fromStdString(command.meta.operationId);
            if (!recoveryOutstanding.contains(operationId) ||
                recoverySubmitted.contains(operationId)) continue;
            const LocalOperationResult submitted =
                bridge->resubmitJournaled(command);
            if (submitted.code == LocalOperationCode::Submitted ||
                submitted.code == LocalOperationCode::Duplicate) {
                recoverySubmitted.insert(operationId);
                continue;
            }
            if (submitted.code == LocalOperationCode::TransportUnavailable)
                return;
            bridge->requireResync(QStringLiteral(
                "A recovered pending edit requires project resync"));
            return;
        }
    }

    void recoveryObserved(const QString& operationId) {
        if (!recoveryOutstanding.remove(operationId)) return;
        recoverySubmitted.remove(operationId);
        if (!recoveryOutstanding.isEmpty()) return;
        recoveryCommands.clear();
        service->setPendingRecoveryBlocked(false);
    }

    void recoveryRejected(const QString& operationId) {
        if (!recoveryOutstanding.contains(operationId)) return;
        bridge->requireResync(QStringLiteral(
            "A recovered pending edit conflicted with the server"));
    }

    void install(quint64 taskGeneration, Materialization result,
                 bool offline) {
        if (taskGeneration != generation ||
            phase != CloudSyncPhase::ReplayingOperations) {
            return;
        }
        bootstrap.reset();
        if (!result.accepted) {
            fail(std::move(result.error));
            return;
        }
        const QString previousProjectId = service->projectId();
        const quint64 previousSequence = service->bootstrapServerSequence();
        const QString previousHash = service->bootstrapStateHash();
        const bool previousRecoveryBlocked =
            service->pendingRecoveryBlocked();
        const bool bindingChanged = previousProjectId != projectId;
        const auto restorePreviousBinding = [&] {
            if (bindingChanged) {
                if (previousProjectId.isEmpty()) {
                    service->clearProject();
                } else {
                    service->setProjectId(previousProjectId, false);
                    if (!previousHash.isEmpty())
                        service->installVerifiedBootstrapState(
                            previousProjectId, previousSequence, previousHash);
                    else
                        service->installVerifiedBootstrapSequence(
                            previousProjectId, previousSequence);
                    service->reconnectNow();
                }
            }
            service->setPendingRecoveryBlocked(previousRecoveryBlocked);
        };
        // The old binding and engine document remain untouched until the
        // candidate bytes have passed snapshot/replay/hash validation above.
        if (bindingChanged) service->setProjectId(projectId, false);
        if (!service->installVerifiedBootstrapState(
                projectId, result.document.confirmedSequence,
                result.canonicalHash)) {
            restorePreviousBinding();
            fail({CloudSyncErrorCode::InstallRejected,
                  QStringLiteral("Cloud project changed during synchronization"),
                  true});
            return;
        }
        const quint64 canonicalSequence = result.document.confirmedSequence;
        const QList<daw::AssetRef> referencedAssets =
            offline ? QList<daw::AssetRef>{}
                    : collectCloudProjectAssets(result.document.project);
        journalLookups = bridge->journaledOperations(projectId);
        if (journalLookups.size() != bridge->journalEntryCount(projectId)) {
            restorePreviousBinding();
            fail({CloudSyncErrorCode::InstallRejected,
                  QStringLiteral(
                      "Pending operation journal is invalid or incomplete"),
                  false});
            return;
        }
        service->setPendingRecoveryBlocked(!journalLookups.isEmpty());
        const daw::collab::GatewayUpdate installed =
            bridge->replaceConfirmedSnapshot(
                std::move(result.document),
                service->bootstrapServerSequence());
        if (!installed.accepted()) {
            restorePreviousBinding();
            fail({CloudSyncErrorCode::InstallRejected,
                  QStringLiteral("Cloud project could not be installed"), true});
            return;
        }
        if (!offline &&
            (bridge->resyncPending() ||
             service->bootstrapServerSequence() != canonicalSequence)) {
            // The bridge detected a gap or bounded-queue overflow while
            // draining operations that arrived during this bootstrap, or it
            // cleanly advanced past the sequence whose hash we computed. The
            // installed state is valid, but fetch a new exact generation so
            // the hash we announce always belongs to the exposed head.
            bridge->requireResync(
                QStringLiteral("Verifying operations received during resync"));
            setPhase(CloudSyncPhase::Ready);
            begin(projectId, false, true);
            return;
        }
        if (offline && bridge->resyncPending()) {
            fail({CloudSyncErrorCode::InstallRejected,
                  QStringLiteral("Offline project recovery is incomplete"),
                  true});
            return;
        }
        installedCanonicalHash = result.canonicalHash;
        installedAssets = referencedAssets;
        installedRole = offline ? CloudProjectRole::Viewer : bootstrapRole;
        installedStatus = offline ? CloudProjectStatus::ReadOnly
                                  : bootstrapStatus;
        installedOffline = offline;
        if (offline) {
            finishInstalledProject();
            return;
        }
        startJournalReconciliation();
    }

    CloudProjectRole bootstrapRole = CloudProjectRole::Viewer;
};

CloudProjectSyncCoordinator::CloudProjectSyncCoordinator(
    CloudProjectClient* projects, CloudAssetTransferManager* transfers,
    CollaborationCommandBridge* bridge, CollaborationService* service,
    QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(this)) {
    m_impl->projects = projects;
    m_impl->transfers = transfers;
    m_impl->bridge = bridge;
    m_impl->service = service;
    m_impl->snapshotRequests = std::make_unique<SnapshotRequestUploader>(
        service, bridge, transfers, this);
    connect(m_impl->snapshotRequests.get(),
            &SnapshotRequestUploader::snapshotUploadStarted, this,
            &CloudProjectSyncCoordinator::snapshotUploadStarted);
    connect(m_impl->snapshotRequests.get(),
            &SnapshotRequestUploader::snapshotUploadCompleted, this,
            &CloudProjectSyncCoordinator::snapshotUploadCompleted);
    connect(m_impl->snapshotRequests.get(),
            &SnapshotRequestUploader::snapshotUploadFailed, this,
            &CloudProjectSyncCoordinator::snapshotUploadFailed);
    if (projects) {
        connect(projects, &CloudProjectClient::bootstrapCompleted, this,
                [this](quint64 requestId,
                       const CloudProjectBootstrap& bootstrap) {
            m_impl->bootstrapReady(requestId, bootstrap);
        });
        connect(projects, &CloudProjectClient::operationLookupReceived, this,
                [this](quint64 requestId,
                       const CloudOperationLookup& lookup) {
            m_impl->journalLookupReady(requestId, lookup);
        });
        connect(projects, &CloudProjectClient::sessionStateReceived, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudSessionState& state) {
            if (requestId != m_impl->sessionRequest ||
                kind != CloudRequestKind::GetActiveSession) return;
            m_impl->sessionRequest = 0;
            m_impl->setPhase(CloudSyncPhase::Ready);
            if (state.session.projectId == m_impl->projectId &&
                state.session.status != CloudSessionStatus::Ended &&
                m_impl->service) {
                m_impl->service->reconnectNow();
            } else {
                if (m_impl->service)
                    m_impl->service->disconnectFromProject();
                emit noActiveSession(m_impl->projectId);
            }
        });
        connect(projects, &CloudProjectClient::requestFailed, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudClientError& error) {
            if (requestId == m_impl->bootstrapRequest &&
                kind == CloudRequestKind::BootstrapProject) {
                m_impl->bootstrapRequest = 0;
                if (error.code == CloudClientErrorCode::Offline ||
                    error.code == CloudClientErrorCode::NetworkFailure ||
                    error.code == CloudClientErrorCode::Timeout) {
                    m_impl->launchOfflineCache();
                } else {
                    m_impl->fail({CloudSyncErrorCode::BootstrapFailed,
                                  error.safeMessage, error.retryable});
                }
            } else if (requestId == m_impl->sessionRequest &&
                       kind == CloudRequestKind::GetActiveSession) {
                m_impl->sessionRequest = 0;
                m_impl->setPhase(CloudSyncPhase::Ready);
                if (provesNoActiveSession(error)) {
                    if (m_impl->service)
                        m_impl->service->disconnectFromProject();
                    emit noActiveSession(m_impl->projectId);
                } else {
                    emit activeSessionCheckFailed(
                        m_impl->projectId,
                        QStringLiteral("Active session could not be verified"),
                        error.retryable);
                }
            } else if (requestId == m_impl->journalLookupRequest &&
                       kind == CloudRequestKind::LookupOperation) {
                m_impl->journalLookupFailed(requestId, error);
            }
        });
    }
    if (transfers) {
        connect(transfers,
                &CloudAssetTransferManager::snapshotDownloadCompleted,
                this, [this](quint64 transferId,
                             const CloudSnapshotDownloadResult& result) {
            m_impl->snapshotReady(transferId, result);
        });
        connect(transfers, &CloudAssetTransferManager::transferFailed, this,
                [this](quint64 transferId, CloudTransferKind kind,
                       const CloudTransferError& error) {
            if (transferId != m_impl->snapshotTransfer ||
                kind != CloudTransferKind::SnapshotDownload) return;
            m_impl->snapshotTransfer = 0;
            m_impl->fail({CloudSyncErrorCode::SnapshotDownloadFailed,
                          error.safeMessage, error.retryable});
        });
    }
    if (service) {
        connect(service, &CollaborationService::hashRoundRequested, this,
                [this](const QString& roundId, const QString& sessionId,
                       quint64 serverSequence, qint64 deadlineMs) {
            m_impl->computeRequestedHash(roundId, sessionId, serverSequence,
                                         deadlineMs);
        });
        connect(service, &CollaborationService::stateChanged, this,
                [this](CollaborationState, const QString&) {
            m_impl->attemptRecoverySubmit();
        });
        connect(service, &CollaborationService::resyncRequired, this,
                [this](const QJsonObject&) {
            if (!m_impl->service || m_impl->service->projectId().isEmpty())
                return;
            m_impl->begin(m_impl->service->projectId(), false, true);
        });
    }
    if (bridge) {
        connect(bridge,
                &CollaborationCommandBridge::operationDurablyObserved,
                this, [this](const QString& operationId, quint64, bool) {
            m_impl->recoveryObserved(operationId);
        });
        connect(bridge, &CollaborationCommandBridge::operationRejected,
                this, [this](const QString& operationId, const QString&,
                             const QString&) {
            m_impl->recoveryRejected(operationId);
        });
        connect(bridge, &CollaborationCommandBridge::pendingOperationsDropped,
                this, [this](const QStringList& operationIds) {
            for (const QString& operationId : operationIds) {
                if (!m_impl->recoveryOutstanding.contains(operationId))
                    continue;
                m_impl->bridge->requireResync(QStringLiteral(
                    "Recovered pending edits require project resync"));
                break;
            }
        });
        connect(bridge, &CollaborationCommandBridge::resyncRequired, this,
                [this](quint64, quint64, const QString&) {
            if (!m_impl->service || m_impl->service->projectId().isEmpty())
                return;
            m_impl->begin(m_impl->service->projectId(), false, true);
        });
    }
}

CloudProjectSyncCoordinator::~CloudProjectSyncCoordinator() {
    m_impl->cancelRequests();
}

quint64 CloudProjectSyncCoordinator::synchronize(const QString& projectId,
                                                 bool connectIfLive) {
    return m_impl->begin(projectId, connectIfLive);
}

void CloudProjectSyncCoordinator::cancel() {
    m_impl->cancelRequests();
    ++m_impl->generation;
    m_impl->setPhase(CloudSyncPhase::Idle);
}

CloudSyncPhase CloudProjectSyncCoordinator::phase() const noexcept {
    return m_impl->phase;
}

QString CloudProjectSyncCoordinator::projectId() const {
    return m_impl->projectId;
}

quint64 CloudProjectSyncCoordinator::generation() const noexcept {
    return m_impl->generation;
}

bool checkCloudProjectSyncCoordinatorForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!checkCloudProjectPublisherForTest(error) ||
        !checkCloudProjectAssetHydratorForTest(error) ||
        !checkSnapshotRequestUploaderForTest(error)) return false;
    CloudClientError absentSession;
    absentSession.httpStatus = 404;
    absentSession.apiCode = QStringLiteral("collaboration_not_found");
    CloudClientError failedSessionCheck;
    failedSessionCheck.code = CloudClientErrorCode::NetworkFailure;
    failedSessionCheck.retryable = true;
    if (!provesNoActiveSession(absentSession) ||
        provesNoActiveSession(failedSessionCheck)) {
        return fail(QStringLiteral(
            "active-session check failure was mistaken for an empty room"));
    }
    const QString projectId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    CollaborationService connectionIntent(nullptr);
    connectionIntent.setProjectId(projectId, true);
    connectionIntent.setProjectId(projectId, false);
    if (connectionIntent.state() != CollaborationState::LocalOnly) {
        return fail(QStringLiteral(
            "same-project bootstrap did not clear stale live connection intent"));
    }
    const QString operationId =
        QStringLiteral("22222222-2222-4222-8222-222222222222");

    const QString firstAssetId =
        QStringLiteral("01010101-0101-4101-8101-010101010101");
    const QString secondAssetId =
        QStringLiteral("02020202-0202-4202-8202-020202020202");
    daw::ProjectModel manifestProject;
    daw::TrackModel manifestTrack;
    daw::ClipModel manifestClip;
    manifestClip.asset.assetId = secondAssetId.toStdString();
    daw::TakeModel manifestTake;
    manifestTake.asset.assetId = firstAssetId.toStdString();
    manifestClip.takes.push_back(std::move(manifestTake));
    daw::TakeModel repeatedManifestTake;
    repeatedManifestTake.asset.assetId = secondAssetId.toStdString();
    manifestClip.takes.push_back(std::move(repeatedManifestTake));
    manifestTrack.clips.push_back(std::move(manifestClip));
    manifestProject.tracks.push_back(std::move(manifestTrack));
    const CloudSnapshotAssetManifest collected =
        collectCloudSnapshotAssetManifest(manifestProject);
    if (!collected.accepted ||
        collected.assetIds != QStringList{firstAssetId, secondAssetId} ||
        isCanonicalCloudSnapshotAssetManifest(
            QStringList{secondAssetId, firstAssetId}) ||
        isCanonicalCloudSnapshotAssetManifest(
            QStringList{firstAssetId, firstAssetId})) {
        return fail(QStringLiteral(
            "snapshot asset manifest was not canonical and deterministic"));
    }

    SharedProjectDocument base;
    base.confirmedSequence = 0;
    std::string bytes;
    if (!daw::collab::serializeSharedProjectSnapshot(base, bytes))
        return fail(QStringLiteral("could not encode sync fixture"));
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("snapshot.json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size()) ||
        !file.flush()) {
        return fail(QStringLiteral("could not write sync fixture"));
    }
    file.close();

    daw::collab::ProjectCommand command;
    command.meta.projectId = projectId.toStdString();
    command.meta.operationId = operationId.toStdString();
    command.meta.actorId =
        "33333333-3333-4333-8333-333333333333";
    command.meta.clientId =
        "44444444-4444-4444-8444-444444444444";
    command.meta.baseServerSequence = 0;
    command.meta.serverSequence = 1;
    command.body = daw::collab::SetProjectScalar{
        daw::collab::ProjectScalar::Tempo, 137.0};

    SharedProjectDocument expected = base;
    if (!ProjectReducer::apply(expected, command).accepted())
        return fail(QStringLiteral("could not apply sync fixture"));
    expected.confirmedSequence = 1;

    CloudProjectBootstrap bootstrap;
    bootstrap.project.id = projectId;
    bootstrap.project.formatVersion = daw::ProjectSerializer::kFormatVersion;
    bootstrap.project.headSequence = 1;
    bootstrap.project.snapshotSequence = 0;
    bootstrap.snapshot = CloudSnapshotDescriptor{
        QStringLiteral("55555555-5555-4555-8555-555555555555"),
        projectId, 0,
        QStringLiteral("66666666-6666-4666-8666-666666666666"),
        daw::ProjectSerializer::kFormatVersion};
    CloudProjectOperation operation;
    operation.projectId = projectId;
    operation.serverSequence = 1;
    operation.command = command;
    bootstrap.operations.push_back(operation);
    bootstrap.replayBaseSequence = 0;
    bootstrap.headSequence = 1;
    for (const auto& [field, writer] : expected.lastWriterByField) {
        CloudProjectFieldHead head;
        head.projectId = projectId;
        head.fieldKey = QString::fromStdString(field);
        head.headSequence = 1;
        head.operationId = QString::fromStdString(writer);
        bootstrap.fieldHeads.push_back(std::move(head));
    }

    Materialization result = materialize(bootstrap, path);
    if (!result.accepted || result.document.confirmedSequence != 1 ||
        result.document.project.tempo != 137.0 ||
        result.canonicalHash.size() != 64) {
        return fail(QStringLiteral("verified bootstrap did not materialize"));
    }

    // Crash before commit: an authoritative absence at the installed head
    // resubmits the original opId. Crash after commit: only the exact command
    // is retired. Reusing the id for any other command is a conflict.
    daw::collab::ProjectCommand journaled = command;
    journaled.meta.actorId.clear();
    journaled.meta.clientId.clear();
    journaled.meta.serverSequence = 0;
    CloudOperationLookup lookup;
    lookup.projectId = projectId;
    lookup.operationId = operationId;
    lookup.headSequence = 1;
    if (decideJournalLookup(projectId, 1, journaled, lookup) !=
        JournalLookupDecision::Resubmit) {
        return fail(QStringLiteral(
            "lost ACK before commit did not preserve the opId for resubmit"));
    }
    operation.actorUserId =
        QString::fromStdString(command.meta.actorId);
    operation.actorDeviceId =
        QString::fromStdString(command.meta.clientId);
    lookup.operation = operation;
    if (decideJournalLookup(projectId, 1, journaled, lookup) !=
        JournalLookupDecision::Retire) {
        return fail(QStringLiteral(
            "lost ACK after commit did not recognize the exact command"));
    }
    const auto conflicts = [&](daw::collab::ProjectCommand changed,
                               const QString& label) {
        CloudOperationLookup mismatched = lookup;
        mismatched.operation->command = std::move(changed);
        if (decideJournalLookup(projectId, 1, journaled, mismatched) ==
            JournalLookupDecision::Resync) {
            return true;
        }
        if (error)
            *error = QStringLiteral("Recovered opId accepted changed %1")
                         .arg(label);
        return false;
    };
    daw::collab::ProjectCommand changed = command;
    changed.body = daw::collab::SetProjectScalar{
        daw::collab::ProjectScalar::Tempo, 138.0};
    if (!conflicts(std::move(changed), QStringLiteral("payload")))
        return false;
    changed = command;
    changed.meta.transactionId =
        "99999999-9999-4999-8999-999999999999";
    if (!conflicts(std::move(changed), QStringLiteral("transaction")))
        return false;
    changed = command;
    changed.meta.baseServerSequence = 1;
    if (!conflicts(std::move(changed), QStringLiteral("base sequence")))
        return false;
    changed = command;
    changed.meta.schemaVersion += 1;
    if (!conflicts(std::move(changed), QStringLiteral("schema")))
        return false;
    changed = command;
    changed.conditions.push_back(daw::collab::FieldWriterIs{
        "project:tempo", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"});
    if (!conflicts(std::move(changed), QStringLiteral("preconditions")))
        return false;
    CloudOperationLookup wrongActor = lookup;
    wrongActor.operation->actorUserId =
        QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    if (decideJournalLookup(projectId, 1, journaled, wrongActor) !=
        JournalLookupDecision::Resync) {
        return fail(QStringLiteral(
            "Recovered opId accepted inconsistent actor identity"));
    }

    bootstrap.snapshot->assetIds = QStringList{
        QStringLiteral("88888888-8888-4888-8888-888888888888")};
    if (materialize(bootstrap, path).accepted)
        return fail(QStringLiteral("forged snapshot asset manifest was accepted"));
    bootstrap.snapshot->assetIds.clear();
    bootstrap.fieldHeads.front().operationId =
        QStringLiteral("77777777-7777-4777-8777-777777777777");
    if (materialize(bootstrap, path).accepted)
        return fail(QStringLiteral("forged field head was accepted"));
    return true;
}

} // namespace collab
