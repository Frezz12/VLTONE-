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
#include "collaboration/ProjectReducer.hpp"
#include "collaboration/SharedProjectSnapshot.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QPointer>
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
    bool connectIfLive = true;
    bool resumeConnectedRoom = false;
    std::shared_ptr<CloudProjectBootstrap> bootstrap;
    std::unique_ptr<SnapshotRequestUploader> snapshotRequests;

    explicit Impl(CloudProjectSyncCoordinator* owner) : q(owner) {
        snapshotDirectory = QDir(QStandardPaths::writableLocation(
                                     QStandardPaths::CacheLocation))
                                .filePath(QStringLiteral(
                                    "collaboration/snapshot-staging-v1"));
        if (!QDir().mkpath(snapshotDirectory)) snapshotDirectory.clear();
        projectCacheRoot = CloudProjectCache::defaultRootDirectory();
    }

    bool active() const {
        return phase == CloudSyncPhase::FetchingBootstrap ||
               phase == CloudSyncPhase::DownloadingSnapshot ||
               phase == CloudSyncPhase::ReplayingOperations ||
               phase == CloudSyncPhase::CheckingLiveSession;
    }

    void setPhase(CloudSyncPhase value) {
        if (phase == value) return;
        phase = value;
        emit q->phaseChanged(phase);
    }

    void cancelRequests() {
        if (projects && bootstrapRequest != 0)
            projects->cancel(bootstrapRequest);
        if (projects && sessionRequest != 0)
            projects->cancel(sessionRequest);
        if (transfers && snapshotTransfer != 0)
            transfers->cancel(snapshotTransfer);
        bootstrapRequest = 0;
        snapshotTransfer = 0;
        sessionRequest = 0;
        bootstrap.reset();
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
        if (active() && projectId == normalized) return generation;

        cancelRequests();
        ++generation;
        projectId = normalized;
        connectIfLive = shouldConnect;
        resumeConnectedRoom = fromResync;
        if (service->projectId() != projectId)
            service->setProjectId(projectId, false);
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
        if (!service->installVerifiedBootstrapState(
                projectId, result.document.confirmedSequence,
                result.canonicalHash)) {
            fail({CloudSyncErrorCode::InstallRejected,
                  QStringLiteral("Cloud project changed during synchronization"),
                  true});
            return;
        }
        const quint64 canonicalSequence = result.document.confirmedSequence;
        const QList<daw::AssetRef> referencedAssets =
            offline ? QList<daw::AssetRef>{}
                    : collectCloudProjectAssets(result.document.project);
        const daw::collab::GatewayUpdate installed =
            bridge->replaceConfirmedSnapshot(
                std::move(result.document),
                service->bootstrapServerSequence());
        if (!installed.accepted()) {
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
        emit q->synchronizedProject(
            projectId, service->bootstrapServerSequence(),
            result.canonicalHash,
            offline ? CloudProjectRole::Viewer : bootstrapRole);
        if (!offline)
            emit q->projectAssetsDiscovered(projectId, referencedAssets);

        if (offline) {
            service->trustedOfflineProjectOpened();
            setPhase(CloudSyncPhase::Ready);
            emit q->noActiveSession(projectId);
            return;
        }

        if (resumeConnectedRoom) {
            setPhase(CloudSyncPhase::Ready);
            service->sendSnapshotHash();
            return;
        }
        if (!connectIfLive) {
            setPhase(CloudSyncPhase::Ready);
            return;
        }
        setPhase(CloudSyncPhase::CheckingLiveSession);
        sessionRequest = projects->getActiveSession(projectId);
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
                    emit noActiveSession(m_impl->projectId);
                } else {
                    emit activeSessionCheckFailed(
                        m_impl->projectId,
                        QStringLiteral("Active session could not be verified"),
                        error.retryable);
                }
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
        connect(service, &CollaborationService::resyncRequired, this,
                [this](const QJsonObject&) {
            if (!m_impl->service || m_impl->service->projectId().isEmpty())
                return;
            m_impl->begin(m_impl->service->projectId(), false, true);
        });
    }
    if (bridge) {
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
