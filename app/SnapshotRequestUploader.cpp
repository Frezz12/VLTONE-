#include "SnapshotRequestUploader.hpp"

#include "CloudAssetTransferManager.hpp"
#include "CloudSnapshotAssetManifest.hpp"
#include "CollaborationCommandBridge.hpp"
#include "CollaborationService.hpp"
#include "ProjectSerializer.hpp"
#include "collaboration/SharedProjectSnapshot.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QQueue>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>

#include <functional>
#include <limits>
#include <memory>
#include <string>

namespace collab {
namespace {

constexpr qsizetype kMaximumTrackedRequests = 128;
constexpr qsizetype kMaximumQueuedRequests = 32;

QString canonicalUuid(const QString& value) {
    const QUuid uuid(value);
    if (uuid.isNull()) return {};
    const QString canonical =
        uuid.toString(QUuid::WithoutBraces).toLower();
    return value == canonical ? canonical : QString();
}

void removeFileOnWorker(QString path) {
    if (path.isEmpty()) return;
    QThreadPool::globalInstance()->start(
        [path = std::move(path)] { QFile::remove(path); });
}

struct PreparedSnapshot {
    bool accepted = false;
    QString safeError;
    QString path;
    QString sha256;
    quint64 byteSize = 0;
    quint64 sequence = 0;
    QStringList assetIds;
};

PreparedSnapshot prepareSnapshot(
    daw::collab::SharedProjectDocument document, const QString& directory,
    const QString& filename) {
    PreparedSnapshot prepared;
    prepared.sequence = document.confirmedSequence;
    const CloudSnapshotAssetManifest manifest =
        collectCloudSnapshotAssetManifest(document.project);
    if (!manifest.accepted) {
        prepared.safeError = manifest.safeError;
        return prepared;
    }
    prepared.assetIds = manifest.assetIds;
    if (directory.isEmpty() || filename.isEmpty() ||
        !QDir().mkpath(directory)) {
        prepared.safeError = QStringLiteral(
            "Snapshot staging directory is unavailable");
        return prepared;
    }

    std::string encoded;
    const audio::Result serialized =
        daw::collab::serializeSharedProjectSnapshot(document, encoded);
    if (!serialized || encoded.empty() ||
        encoded.size() > daw::collab::kMaximumSharedProjectSnapshotBytes) {
        prepared.safeError = QStringLiteral(
            "Confirmed project cannot be serialized safely");
        return prepared;
    }
    const QByteArray bytes(encoded.data(), qsizetype(encoded.size()));
    prepared.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    prepared.byteSize = quint64(bytes.size());
    prepared.path = QDir(directory).filePath(filename);

    QSaveFile file(prepared.path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit() ||
        !QFile::setPermissions(prepared.path,
                               QFileDevice::ReadOwner |
                                   QFileDevice::WriteOwner)) {
        QFile::remove(prepared.path);
        prepared.path.clear();
        prepared.safeError = QStringLiteral(
            "Confirmed project snapshot could not be staged");
        return prepared;
    }
    prepared.accepted = true;
    return prepared;
}

} // namespace

struct SnapshotRequestUploader::Impl {
    enum class Status : quint8 {
        Queued,
        Preparing,
        Uploading,
        WaitingRetry,
        Completed,
        TerminalFailure,
    };

    struct Entry {
        SnapshotRequest request;
        QString projectId;
        int highestAttempt = 0;
        Status status = Status::Queued;
        QString preparedPath;
        QString sha256;
        quint64 byteSize = 0;
        QStringList assetIds;
    };

    SnapshotRequestUploader* q = nullptr;
    QPointer<CollaborationService> service;
    QPointer<CollaborationCommandBridge> bridge;
    QPointer<CloudAssetTransferManager> transfers;
    std::function<quint64(const CloudSnapshotUploadInput&)> startUpload;
    std::function<bool(quint64)> cancelTransfer;
    QHash<QString, Entry> tracked;
    QQueue<QString> queue;
    QList<QString> history;
    QHash<quint64, QString> cancelledTransferPaths;
    QString activeRequestId;
    int activeAttempt = 0;
    quint64 activeTransferId = 0;
    QString activeUploadId;
    quint64 epoch = 1;
    QString stagingDirectory;

    explicit Impl(SnapshotRequestUploader* owner) : q(owner) {
        stagingDirectory = QDir(QStandardPaths::writableLocation(
                                    QStandardPaths::CacheLocation))
                               .filePath(QStringLiteral(
                                   "collaboration/snapshot-upload-staging-v1"));
    }

    bool contextMatches(const Entry& entry) const {
        return service && bridge && !entry.projectId.isEmpty() &&
               service->projectId() == entry.projectId &&
               service->sessionId() == entry.request.sessionId &&
               service->localParticipantId() ==
                   entry.request.hostParticipantId &&
               service->hostParticipantId() ==
                   entry.request.hostParticipantId;
    }

    static bool sameImmutableRequest(const Entry& entry,
                                     const SnapshotRequest& request,
                                     const QString& projectId) {
        return entry.projectId == projectId &&
               entry.request.requestId == request.requestId &&
               entry.request.sessionId == request.sessionId &&
               entry.request.hostParticipantId ==
                   request.hostParticipantId &&
               entry.request.targetServerSequence ==
                   request.targetServerSequence &&
               entry.request.reason == request.reason;
    }

    void removeTracked(const QString& requestId) {
        auto iterator = tracked.find(requestId);
        if (iterator == tracked.end()) return;
        removeFileOnWorker(iterator->preparedPath);
        tracked.erase(iterator);
        history.removeAll(requestId);
        queue.removeAll(requestId);
    }

    bool makeTrackingRoom() {
        while (tracked.size() >= kMaximumTrackedRequests) {
            QString removable;
            for (const QString& requestId : history) {
                const auto iterator = tracked.constFind(requestId);
                if (iterator == tracked.cend() ||
                    requestId == activeRequestId ||
                    queue.contains(requestId)) {
                    continue;
                }
                if (iterator->status == Status::Completed ||
                    iterator->status == Status::TerminalFailure) {
                    removable = requestId;
                    break;
                }
            }
            if (removable.isEmpty()) return false;
            removeTracked(removable);
        }
        return true;
    }

    void emitFailure(const Entry& entry, const QString& message,
                     bool retryable) {
        emit q->snapshotUploadFailed(
            entry.request.requestId, entry.request.targetServerSequence,
            message, retryable);
    }

    void receive(const SnapshotRequest& request) {
        const QString projectId = service ? service->projectId() : QString();
        if (canonicalUuid(request.requestId).isEmpty() ||
            canonicalUuid(request.sessionId).isEmpty() ||
            canonicalUuid(request.hostParticipantId).isEmpty() ||
            projectId.isEmpty() || request.attempt < 1 ||
            !service || service->sessionId() != request.sessionId ||
            service->localParticipantId() != request.hostParticipantId ||
            service->hostParticipantId() != request.hostParticipantId) {
            emit q->snapshotUploadFailed(
                request.requestId, request.targetServerSequence,
                QStringLiteral("Snapshot request no longer belongs to this host"),
                false);
            return;
        }

        auto iterator = tracked.find(request.requestId);
        if (iterator != tracked.end()) {
            if (!sameImmutableRequest(*iterator, request, projectId)) {
                emitFailure(*iterator,
                            QStringLiteral(
                                "Snapshot request identity changed unexpectedly"),
                            false);
                return;
            }
            if (request.attempt <= iterator->highestAttempt ||
                iterator->status == Status::Completed ||
                iterator->status == Status::TerminalFailure) {
                return;
            }
            iterator->highestAttempt = request.attempt;
            iterator->request.attempt = request.attempt;
            iterator->request.retryAtMs = request.retryAtMs;
            if (request.requestId != activeRequestId &&
                !queue.contains(request.requestId)) {
                if (queue.size() >= kMaximumQueuedRequests) {
                    emitFailure(*iterator,
                                QStringLiteral(
                                    "Too many snapshot requests are pending"),
                                true);
                    return;
                }
                iterator->status = Status::Queued;
                queue.enqueue(request.requestId);
            }
            pump();
            return;
        }

        if (!makeTrackingRoom() || queue.size() >= kMaximumQueuedRequests) {
            emit q->snapshotUploadFailed(
                request.requestId, request.targetServerSequence,
                QStringLiteral("Snapshot request capacity is exhausted"), true);
            return;
        }
        Entry entry;
        entry.request = request;
        entry.projectId = projectId;
        entry.highestAttempt = request.attempt;
        tracked.insert(request.requestId, entry);
        history.push_back(request.requestId);
        queue.enqueue(request.requestId);
        pump();
    }

    void pump() {
        if (!activeRequestId.isEmpty() || !service || !bridge) return;
        const qsizetype candidates = queue.size();
        for (qsizetype index = 0; index < candidates; ++index) {
            const QString requestId = queue.dequeue();
            auto iterator = tracked.find(requestId);
            if (iterator == tracked.end() ||
                iterator->status == Status::Completed ||
                iterator->status == Status::TerminalFailure) {
                continue;
            }
            if (!contextMatches(*iterator)) {
                iterator->status = Status::TerminalFailure;
                emitFailure(*iterator,
                            QStringLiteral(
                                "Snapshot upload was cancelled after host change"),
                            false);
                continue;
            }
            if (!iterator->preparedPath.isEmpty()) {
                activeRequestId = requestId;
                activeAttempt = iterator->request.attempt;
                startPreparedUpload();
                return;
            }

            const quint64 confirmed = bridge->confirmedServerSequence();
            if (service->bootstrapServerSequence() != confirmed) {
                queue.enqueue(requestId);
                bridge->requireResync(QStringLiteral(
                    "Snapshot upload requires one verified confirmed sequence"));
                return;
            }
            if (confirmed < iterator->request.targetServerSequence) {
                queue.enqueue(requestId);
                continue;
            }
            if (confirmed > iterator->request.targetServerSequence) {
                iterator->status = Status::TerminalFailure;
                const QString message = QStringLiteral(
                    "The requested snapshot sequence is older than the "
                    "confirmed project; a safe resync is required");
                emitFailure(*iterator, message, false);
                bridge->requireResync(message);
                continue;
            }
            auto snapshot = bridge->confirmedSnapshotAt(confirmed);
            if (!snapshot) {
                // The exact sequence exists but is currently resync-blocked.
                // stateChanged will pump again after verified installation.
                queue.enqueue(requestId);
                continue;
            }
            activeRequestId = requestId;
            activeAttempt = iterator->request.attempt;
            iterator->status = Status::Preparing;
            const quint64 taskEpoch = epoch;
            const QString filename = QStringLiteral("%1-%2.snapshot")
                .arg(requestId,
                     QUuid::createUuid().toString(QUuid::WithoutBraces));
            const QString directory = stagingDirectory;
            QPointer<SnapshotRequestUploader> guard(q);
            QThreadPool::globalInstance()->start(
                [guard, taskEpoch, requestId, directory, filename,
                 snapshot = std::move(*snapshot)]() mutable {
                    auto result = std::make_shared<PreparedSnapshot>(
                        prepareSnapshot(std::move(snapshot), directory,
                                        filename));
                    if (!guard) {
                        removeFileOnWorker(result->path);
                        return;
                    }
                    QMetaObject::invokeMethod(
                        guard.data(),
                        [guard, taskEpoch, requestId, result] {
                            if (guard)
                                guard->m_impl->prepared(
                                    taskEpoch, requestId, std::move(*result));
                            else
                                removeFileOnWorker(result->path);
                        },
                        Qt::QueuedConnection);
                });
            return;
        }
    }

    void prepared(quint64 taskEpoch, const QString& requestId,
                  PreparedSnapshot result) {
        auto iterator = tracked.find(requestId);
        if (taskEpoch != epoch || requestId != activeRequestId ||
            iterator == tracked.end() || !contextMatches(*iterator)) {
            removeFileOnWorker(std::move(result.path));
            if (requestId == activeRequestId) {
                activeRequestId.clear();
                activeAttempt = 0;
                pump();
            }
            return;
        }
        if (!result.accepted ||
            result.sequence != iterator->request.targetServerSequence) {
            iterator->status = Status::TerminalFailure;
            emitFailure(*iterator,
                        result.safeError.isEmpty()
                            ? QStringLiteral(
                                  "Confirmed snapshot sequence changed")
                            : result.safeError,
                        false);
            activeRequestId.clear();
            activeAttempt = 0;
            pump();
            return;
        }
        iterator->preparedPath = std::move(result.path);
        iterator->sha256 = std::move(result.sha256);
        iterator->byteSize = result.byteSize;
        iterator->assetIds = std::move(result.assetIds);
        startPreparedUpload();
    }

    void startPreparedUpload() {
        auto iterator = tracked.find(activeRequestId);
        if (iterator == tracked.end() || !contextMatches(*iterator) ||
            iterator->preparedPath.isEmpty() || iterator->sha256.isEmpty() ||
            iterator->byteSize == 0 || !startUpload) {
            if (iterator != tracked.end()) {
                iterator->status = Status::WaitingRetry;
                emitFailure(*iterator,
                            QStringLiteral("Snapshot upload is unavailable"),
                            true);
            }
            activeRequestId.clear();
            activeAttempt = 0;
            return;
        }
        activeUploadId =
            QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
        CloudSnapshotUploadInput input;
        input.projectId = iterator->projectId;
        input.uploadId = activeUploadId;
        input.sourcePath = iterator->preparedPath;
        input.sequence = iterator->request.targetServerSequence;
        input.schemaVersion = daw::ProjectSerializer::kFormatVersion;
        input.sha256 = iterator->sha256;
        input.byteSize = iterator->byteSize;
        input.assetIds = iterator->assetIds;
        iterator->status = Status::Uploading;
        activeTransferId = startUpload(input);
        if (activeTransferId == 0) {
            iterator->status = Status::WaitingRetry;
            emitFailure(*iterator,
                        QStringLiteral("Snapshot upload could not be started"),
                        true);
            activeRequestId.clear();
            activeAttempt = 0;
            activeUploadId.clear();
            return;
        }
        emit q->snapshotUploadStarted(
            iterator->request.requestId, activeAttempt,
            iterator->request.targetServerSequence);
    }

    void uploadSucceeded(quint64 transferId,
                         const CloudSnapshotUploadResult& result) {
        const auto cancelled = cancelledTransferPaths.find(transferId);
        if (cancelled != cancelledTransferPaths.end()) {
            removeFileOnWorker(cancelled.value());
            cancelledTransferPaths.erase(cancelled);
            return;
        }
        if (transferId != activeTransferId || activeRequestId.isEmpty()) return;
        auto iterator = tracked.find(activeRequestId);
        if (iterator == tracked.end()) {
            activeTransferId = 0;
            activeRequestId.clear();
            return;
        }
        const bool exact = result.projectId == iterator->projectId &&
                           result.uploadId == activeUploadId &&
                           result.sequence ==
                               iterator->request.targetServerSequence &&
                           result.schemaVersion ==
                               daw::ProjectSerializer::kFormatVersion &&
                           result.sha256 == iterator->sha256 &&
                           result.byteSize == iterator->byteSize &&
                           result.assetIds == iterator->assetIds;
        if (!exact) {
            finishTransferFailure(
                {CloudTransferErrorCode::InvalidResponse, 0, {},
                 QStringLiteral("Snapshot completion did not match its request"),
                 {}, false});
            return;
        }
        const QString requestId = iterator->request.requestId;
        const quint64 sequence = iterator->request.targetServerSequence;
        iterator->status = Status::Completed;
        removeFileOnWorker(std::move(iterator->preparedPath));
        activeTransferId = 0;
        activeRequestId.clear();
        activeAttempt = 0;
        activeUploadId.clear();
        emit q->snapshotUploadCompleted(requestId, sequence);
        pump();
    }

    void transferFailed(quint64 transferId, CloudTransferKind kind,
                        const CloudTransferError& error) {
        const auto cancelled = cancelledTransferPaths.find(transferId);
        if (cancelled != cancelledTransferPaths.end()) {
            removeFileOnWorker(cancelled.value());
            cancelledTransferPaths.erase(cancelled);
            return;
        }
        if (kind != CloudTransferKind::SnapshotUpload ||
            transferId != activeTransferId) {
            return;
        }
        finishTransferFailure(error);
    }

    void finishTransferFailure(const CloudTransferError& error) {
        auto iterator = tracked.find(activeRequestId);
        if (iterator == tracked.end()) return;
        const quint64 failedTransfer = activeTransferId;
        const int failedAttempt = activeAttempt;
        activeTransferId = 0;
        activeRequestId.clear();
        activeAttempt = 0;
        activeUploadId.clear();
        if (failedTransfer != 0 && cancelTransfer) {
            cancelledTransferPaths.insert(failedTransfer, {});
            cancelTransfer(failedTransfer);
        }
        const QString message = error.safeMessage.isEmpty()
            ? QStringLiteral("Snapshot upload failed")
            : error.safeMessage;
        if (error.retryable) {
            iterator->status = Status::WaitingRetry;
            emitFailure(*iterator, message, true);
            // A newer server dispatch may already have arrived while the old
            // transfer was in flight. Reuse the immutable exact bytes but use
            // a fresh uploadId for that new attempt.
            if (iterator->highestAttempt > failedAttempt) {
                iterator->status = Status::Queued;
                queue.enqueue(iterator->request.requestId);
            }
        } else {
            iterator->status = Status::TerminalFailure;
            emitFailure(*iterator, message, false);
            removeFileOnWorker(std::move(iterator->preparedPath));
        }
        pump();
    }

    void cancelAll() {
        ++epoch;
        queue.clear();
        if (activeTransferId != 0) {
            QString path;
            const auto iterator = tracked.constFind(activeRequestId);
            if (iterator != tracked.cend()) path = iterator->preparedPath;
            cancelledTransferPaths.insert(activeTransferId, path);
            if (cancelTransfer) cancelTransfer(activeTransferId);
        }
        for (auto iterator = tracked.begin(); iterator != tracked.end();
             ++iterator) {
            if (iterator.key() != activeRequestId || activeTransferId == 0)
                removeFileOnWorker(iterator->preparedPath);
        }
        tracked.clear();
        history.clear();
        activeRequestId.clear();
        activeAttempt = 0;
        activeTransferId = 0;
        activeUploadId.clear();
    }
};

SnapshotRequestUploader::SnapshotRequestUploader(
    CollaborationService* service, CollaborationCommandBridge* bridge,
    CloudAssetTransferManager* transfers, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(this)) {
    m_impl->service = service;
    m_impl->bridge = bridge;
    m_impl->transfers = transfers;
    if (transfers) {
        QPointer<CloudAssetTransferManager> guard(transfers);
        m_impl->startUpload = [guard](const CloudSnapshotUploadInput& input) {
            return guard ? guard->uploadSnapshot(input) : 0;
        };
        m_impl->cancelTransfer = [guard](quint64 transferId) {
            return guard && guard->cancel(transferId);
        };
        connect(transfers,
                &CloudAssetTransferManager::snapshotUploadCompleted, this,
                [this](quint64 transferId,
                       const CloudSnapshotUploadResult& result) {
                    m_impl->uploadSucceeded(transferId, result);
                });
        connect(transfers, &CloudAssetTransferManager::transferFailed, this,
                [this](quint64 transferId, CloudTransferKind kind,
                       const CloudTransferError& error) {
                    m_impl->transferFailed(transferId, kind, error);
                });
    }
    if (service) {
        connect(service, &CollaborationService::snapshotRequested, this,
                [this](const SnapshotRequest& request) {
                    m_impl->receive(request);
                });
        connect(service, &CollaborationService::projectChanged, this,
                [this](const QString&) { m_impl->cancelAll(); });
        connect(service, &CollaborationService::roomIdentityChanged, this,
                [this](const QString&, const QString&, const QString&) {
                    if (m_impl->activeRequestId.isEmpty() &&
                        m_impl->queue.isEmpty()) {
                        return;
                    }
                    bool valid = true;
                    for (auto iterator = m_impl->tracked.cbegin();
                         iterator != m_impl->tracked.cend(); ++iterator) {
                        if (iterator->status != Impl::Status::Completed &&
                            iterator->status != Impl::Status::TerminalFailure &&
                            !m_impl->contextMatches(*iterator)) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) m_impl->cancelAll();
                });
        connect(service, &CollaborationService::stateChanged, this,
                [this](CollaborationState state, const QString&) {
                    if (state == CollaborationState::Synced ||
                        state == CollaborationState::ReadOnly)
                        m_impl->pump();
                });
    }
    if (bridge) {
        connect(bridge, &CollaborationCommandBridge::operationCommitted, this,
                [this](const QString&, quint64, bool) { m_impl->pump(); });
    }
}

SnapshotRequestUploader::~SnapshotRequestUploader() {
    m_impl->cancelAll();
}

void SnapshotRequestUploader::cancel() {
    m_impl->cancelAll();
}

bool checkSnapshotRequestUploaderForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const QString projectId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QString sessionId =
        QStringLiteral("22222222-2222-4222-8222-222222222222");
    const QString hostId =
        QStringLiteral("33333333-3333-4333-8333-333333333333");
    const QString requestId =
        QStringLiteral("44444444-4444-4444-8444-444444444444");

    daw::collab::SharedProjectDocument confirmed;
    confirmed.confirmedSequence = 7;
    confirmed.project.tempo = 120.0;
    const QString snapshotAssetId =
        QStringLiteral("10101010-1010-4010-8010-101010101010");
    daw::TrackModel snapshotTrack;
    snapshotTrack.id = "20202020-2020-4020-8020-202020202020";
    daw::ClipModel snapshotClip;
    snapshotClip.id = "30303030-3030-4030-8030-303030303030";
    snapshotClip.kind = daw::ClipKind::Audio;
    snapshotClip.asset.assetId = snapshotAssetId.toStdString();
    snapshotClip.asset.sha256 = std::string(64, 'a');
    snapshotClip.asset.kind = daw::AssetKind::Audio;
    snapshotClip.asset.byteSize = 4;
    snapshotClip.asset.originalName = "take.wav";
    snapshotClip.asset.mimeType = "audio/wav";
    snapshotTrack.clips.push_back(std::move(snapshotClip));
    confirmed.project.tracks.push_back(std::move(snapshotTrack));
    daw::collab::CommandGateway gateway(confirmed);
    CollaborationService service(nullptr);
    CollaborationCommandBridge bridge(&service, &gateway);
    QTemporaryDir snapshotStaging;
    if (!snapshotStaging.isValid())
        return fail(QStringLiteral("snapshot test staging is unavailable"));
    SnapshotRequestUploader uploader(&service, &bridge, nullptr);
    // Keep the deterministic self-test independent from the user's real cache
    // directory (and from CI/sandbox permissions). Production instances still
    // use QStandardPaths::CacheLocation.
    uploader.m_impl->stagingDirectory = snapshotStaging.path();

    service.m_projectId = projectId;
    service.m_sessionId = sessionId;
    service.m_transportConnected = true;
    service.m_shouldConnect = true;
    service.m_bootstrapServerSequence = 7;
    service.m_presenceStore.setLocalParticipantId(hostId);
    service.m_localSessionState.setHostParticipantId(hostId);

    // A pending optimistic edit must never enter the uploaded generation.
    daw::collab::ProjectCommand pending;
    pending.meta.projectId = projectId.toStdString();
    pending.meta.operationId =
        "55555555-5555-4555-8555-555555555555";
    pending.meta.actorId =
        "66666666-6666-4666-8666-666666666666";
    pending.meta.clientId =
        "77777777-7777-4777-8777-777777777777";
    pending.meta.baseServerSequence = 7;
    pending.body = daw::collab::SetProjectScalar{
        daw::collab::ProjectScalar::Tempo, 155.0};
    if (!gateway.submit(pending).accepted() ||
        gateway.optimistic().project.tempo != 155.0 ||
        gateway.confirmed().project.tempo != 120.0) {
        return fail(QStringLiteral("snapshot test could not create optimistic state"));
    }

    QList<CloudSnapshotUploadInput> captured;
    int starts = 0;
    QList<quint64> cancelled;
    uploader.m_impl->startUpload =
        [&captured, &starts](const CloudSnapshotUploadInput& input) {
            captured.push_back(input);
            ++starts;
            return quint64(90 + starts);
        };
    uploader.m_impl->cancelTransfer = [&cancelled](quint64 id) {
        cancelled.push_back(id);
        return true;
    };

    const auto envelopeFor = [&](QString assignedHost, int attempt,
                                 bool extra = false) {
        WireEnvelope envelope;
        envelope.type = WireType::SnapshotRequested;
        envelope.messageId =
            QStringLiteral("88888888-8888-4888-8888-888888888888");
        envelope.serverTimeMs = 1;
        envelope.payload = QJsonObject{
            {QStringLiteral("requestId"), requestId},
            {QStringLiteral("sessionId"), sessionId},
            {QStringLiteral("hostParticipantId"), assignedHost},
            {QStringLiteral("targetServerSeq"), 7.0},
            {QStringLiteral("reason"), QStringLiteral("session_end")},
            {QStringLiteral("attempt"), double(attempt)},
            {QStringLiteral("retryAtMs"), 1000.0},
        };
        if (extra) envelope.payload.insert(QStringLiteral("filename"),
                                           QStringLiteral("secret.vlt"));
        return envelope;
    };

    int acceptedRequests = 0;
    QObject::connect(&service, &CollaborationService::snapshotRequested,
                     &service, [&acceptedRequests](const SnapshotRequest&) {
                         ++acceptedRequests;
                     });
    QJsonObject strictWire = wireEnvelopeToJson(envelopeFor(hostId, 1));
    QString parseError;
    if (!wireEnvelopeFromJson(strictWire, &parseError))
        return fail(QStringLiteral("snapshot request was missing from wire allowlist"));
    strictWire.insert(QStringLiteral("filename"),
                      QStringLiteral("private.vlt"));
    if (wireEnvelopeFromJson(strictWire, &parseError))
        return fail(QStringLiteral("extended snapshot envelope was accepted"));
    service.handleEnvelope(envelopeFor(
        QStringLiteral("99999999-9999-4999-8999-999999999999"), 1));
    service.handleEnvelope(envelopeFor(hostId, 1, true));
    if (acceptedRequests != 0 || starts != 0)
        return fail(QStringLiteral("foreign or extended snapshot request was accepted"));

    service.handleEnvelope(envelopeFor(hostId, 1));
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&uploader, &SnapshotRequestUploader::snapshotUploadStarted,
                     &loop, &QEventLoop::quit);
    timeout.start(3000);
    if (starts == 0) loop.exec();
    if (acceptedRequests != 1 || starts != 1 || captured.size() != 1 ||
        captured.front().sequence != 7 ||
        captured.front().schemaVersion !=
            daw::ProjectSerializer::kFormatVersion ||
        canonicalUuid(captured.front().uploadId).isEmpty() ||
        captured.front().sha256.size() != 64 ||
        captured.front().byteSize == 0 ||
        captured.front().sourcePath.isEmpty() ||
        captured.front().assetIds != QStringList{snapshotAssetId}) {
        return fail(QStringLiteral("exact snapshot upload was not started"));
    }
    QFile snapshot(captured.front().sourcePath);
    if (!snapshot.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("staged snapshot was unavailable"));
    const QByteArray staged = snapshot.readAll();
    snapshot.close();
    daw::collab::SharedProjectDocument decoded;
    if (!daw::collab::deserializeSharedProjectSnapshot(
            decoded, std::string_view(staged.constData(), staged.size())) ||
        decoded.confirmedSequence != 7 || decoded.project.tempo != 120.0) {
        return fail(QStringLiteral("snapshot included optimistic project state"));
    }

    // Repeating the same requestId/attempt is a no-op while the first transfer
    // remains the sole in-flight upload.
    service.handleEnvelope(envelopeFor(hostId, 1));
    if (acceptedRequests != 2 || starts != 1)
        return fail(QStringLiteral("snapshot request delivery was not deduplicated"));

    // A higher server attempt arriving while the old transfer is active waits
    // for it, then reuses the immutable exact bytes with a fresh upload UUID.
    service.handleEnvelope(envelopeFor(hostId, 2));
    if (starts != 1) return fail(QStringLiteral("parallel snapshot upload started"));
    CloudTransferError retryable;
    retryable.code = CloudTransferErrorCode::NetworkFailure;
    retryable.safeMessage = QStringLiteral("temporary failure");
    retryable.retryable = true;
    uploader.m_impl->finishTransferFailure(retryable);
    if (starts != 2 || captured.size() != 2 ||
        captured.at(1).sourcePath != captured.at(0).sourcePath ||
        captured.at(1).sha256 != captured.at(0).sha256 ||
        captured.at(1).assetIds != captured.at(0).assetIds ||
        captured.at(1).uploadId == captured.at(0).uploadId ||
        cancelled != QList<quint64>{91}) {
        return fail(QStringLiteral("snapshot retry was not idempotent"));
    }

    service.m_localSessionState.setHostParticipantId(
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"));
    emit service.roomIdentityChanged(
        sessionId, hostId, service.m_localSessionState.hostParticipantId());
    if (cancelled != QList<quint64>{91, 92} ||
        !uploader.m_impl->tracked.isEmpty() ||
        !uploader.m_impl->activeRequestId.isEmpty()) {
        return fail(QStringLiteral("host change did not cancel snapshot upload"));
    }
    CloudTransferError cancelledError;
    cancelledError.code = CloudTransferErrorCode::Cancelled;
    uploader.m_impl->transferFailed(
        92, CloudTransferKind::SnapshotUpload, cancelledError);

    // The delivery ledger retains only a fixed number of terminal request ids
    // for dedupe and deterministically evicts the oldest terminal entry.
    for (qsizetype index = 0;
         index < kMaximumTrackedRequests + 16; ++index) {
        if (!uploader.m_impl->makeTrackingRoom())
            return fail(QStringLiteral("bounded request ledger could not prune"));
        const QString id = QStringLiteral("terminal-%1").arg(index);
        SnapshotRequestUploader::Impl::Entry entry;
        entry.status = SnapshotRequestUploader::Impl::Status::Completed;
        uploader.m_impl->tracked.insert(id, entry);
        uploader.m_impl->history.push_back(id);
    }
    if (uploader.m_impl->tracked.size() > kMaximumTrackedRequests)
        return fail(QStringLiteral("snapshot request ledger was unbounded"));
    return true;
}

} // namespace collab
