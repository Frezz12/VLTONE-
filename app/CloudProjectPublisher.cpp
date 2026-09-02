#include "CloudProjectPublisher.hpp"

#include "CloudSnapshotAssetManifest.hpp"
#include "ProjectSerializer.hpp"
#include "cloud/CloudDocumentProjection.hpp"
#include "cloud/PublishPreflight.hpp"
#include "collaboration/ProjectCommand.hpp"
#include "collaboration/SharedProjectSnapshot.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QQueue>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace collab {
namespace {

constexpr int kDefaultMaximumConcurrentUploads = 3;
constexpr int kMaximumConcurrentUploads = 4;
constexpr qsizetype kMaximumPublicationAssets = 100000;
constexpr quint64 kMaximumBlobBytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr int kMaximumTransferAttempts = 3;
constexpr char kPartialHashPlaceholder[] =
    "0000000000000000000000000000000000000000000000000000000000000000";

QString canonicalUuid(const QString& value) {
    const QUuid parsed(value);
    if (parsed.isNull()) return {};
    const QString canonical =
        parsed.toString(QUuid::WithoutBraces).toLower();
    return value == canonical ? canonical : QString();
}

bool validSha256(const QString& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.cbegin(), value.cend(), [](QChar character) {
        const ushort code = character.unicode();
        return (code >= '0' && code <= '9') ||
               (code >= 'a' && code <= 'f');
    });
}

bool validContentType(const QString& value) {
    return !value.isEmpty() && value.size() <= 160 &&
           !value.contains(QLatin1Char('\r')) &&
           !value.contains(QLatin1Char('\n')) &&
           !value.contains(QChar::Null);
}

QString safeBasename(QString value) {
    value = value.trimmed();
    value.replace(QLatin1Char('\\'), QLatin1Char('/'));
    value = value.section(QLatin1Char('/'), -1);
    if (value.isEmpty() || value == QLatin1String(".") ||
        value == QLatin1String("..") || value.size() > 255 ||
        value.contains(QChar::Null) || value.contains(QLatin1Char('\r')) ||
        value.contains(QLatin1Char('\n'))) {
        return {};
    }
    return value;
}

bool validProjectMetadata(const CreateCloudProjectInput& input) {
    const QString title = input.title.trimmed();
    const QString engine = input.engineVersion.trimmed();
    const QString minimum = input.minimumAppVersion.trimmed();
    return input.formatVersion == daw::ProjectSerializer::kFormatVersion &&
           !title.isEmpty() && title.size() <= 160 && !engine.isEmpty() &&
           engine.size() <= 64 && !minimum.isEmpty() && minimum.size() <= 64;
}

std::optional<CloudAssetKind> transferKind(daw::AssetKind kind) {
    switch (kind) {
        case daw::AssetKind::Audio: return CloudAssetKind::Audio;
        case daw::AssetKind::PluginState:
            return CloudAssetKind::PluginState;
        // V1 Sampler bindings are Audio AssetRefs and are uploaded as audio.
        // The transfer layer maps `sample` to PluginResource, which is not in
        // the current reducer/preflight compatibility set.
        case daw::AssetKind::PluginResource:
        case daw::AssetKind::Freeze:
        case daw::AssetKind::Unknown: break;
    }
    return std::nullopt;
}

void visitInsertAssets(
    daw::InsertModel& insert,
    const std::function<void(daw::AssetRef&, const QString&)>& visitor,
    const QString& location) {
    visitor(insert.stateAsset, location + QStringLiteral("/state"));
    visitor(insert.rightStateAsset,
            location + QStringLiteral("/right-state"));
    for (daw::PluginAssetBinding& binding : insert.assetBindings) {
        visitor(binding.asset,
                location + QStringLiteral("/binding:") +
                    QString::fromStdString(binding.key));
    }
}

void visitInsertListAssets(
    std::vector<daw::InsertModel>& inserts,
    const std::function<void(daw::AssetRef&, const QString&)>& visitor,
    const QString& location) {
    for (std::size_t index = 0; index < inserts.size(); ++index) {
        const QString id = QString::fromStdString(inserts[index].id);
        visitInsertAssets(
            inserts[index], visitor,
            location + QStringLiteral("/insert:") +
                (id.isEmpty() ? QString::number(index) : id));
    }
}

void visitProjectAssets(
    daw::ProjectModel& project,
    const std::function<void(daw::AssetRef&, const QString&)>& visitor) {
    visitInsertListAssets(project.masterInserts, visitor,
                          QStringLiteral("master"));
    for (std::size_t trackIndex = 0; trackIndex < project.tracks.size();
         ++trackIndex) {
        daw::TrackModel& track = project.tracks[trackIndex];
        const QString trackId = QString::fromStdString(track.id);
        const QString trackLocation =
            QStringLiteral("track:") +
            (trackId.isEmpty() ? QString::number(trackIndex) : trackId);
        visitInsertAssets(track.instrument, visitor,
                          trackLocation + QStringLiteral("/instrument"));
        visitInsertListAssets(track.samplerFx.inserts, visitor,
                              trackLocation + QStringLiteral("/sampler-fx"));
        visitInsertListAssets(track.inserts, visitor, trackLocation);
        for (std::size_t clipIndex = 0; clipIndex < track.clips.size();
             ++clipIndex) {
            daw::ClipModel& clip = track.clips[clipIndex];
            const QString clipId = QString::fromStdString(clip.id);
            const QString clipLocation =
                trackLocation + QStringLiteral("/clip:") +
                (clipId.isEmpty() ? QString::number(clipIndex) : clipId);
            visitor(clip.asset, clipLocation + QStringLiteral("/audio"));
            visitInsertListAssets(clip.inserts, visitor, clipLocation);
            for (std::size_t takeIndex = 0; takeIndex < clip.takes.size();
                 ++takeIndex) {
                visitor(clip.takes[takeIndex].asset,
                        clipLocation + QStringLiteral("/take:") +
                            QString::number(takeIndex));
            }
        }
    }
}

struct PreparedAsset {
    daw::AssetRef expected;
    QString sourcePath;
    QString contentType;
    QString displayName;
    CloudAssetKind kind = CloudAssetKind::Audio;
    int referenceCount = 0;
};

struct PreparedPublication {
    daw::ProjectModel document;
    CreateCloudProjectInput metadata;
    QVector<PreparedAsset> assets;
};

struct PreflightOutcome {
    bool accepted = false;
    QString safeError;
    std::shared_ptr<PreparedPublication> publication;
};

PreflightOutcome performPreflight(CloudProjectPublicationInput input) {
    PreflightOutcome outcome;
    const auto reject = [&outcome](QString message) {
        outcome.safeError = std::move(message);
        return outcome;
    };
    if (!validProjectMetadata(input.metadata) ||
        input.assetSources.size() > kMaximumPublicationAssets) {
        return reject(QStringLiteral("Cloud project metadata is invalid"));
    }

    struct Group {
        daw::AssetRef expected;
        int references = 0;
    };
    QVector<Group> groups;
    QHash<QString, int> groupById;
    QString assetFailure;
    visitProjectAssets(
        input.project,
        [&](daw::AssetRef& asset, const QString&) {
            if (!assetFailure.isEmpty() || asset.empty()) return;
            const QString id = QString::fromStdString(asset.assetId);
            const QString sha = QString::fromStdString(asset.sha256);
            if (canonicalUuid(id).isEmpty() ||
                (!sha.isEmpty() && !validSha256(sha)) ||
                asset.byteSize == 0 || asset.byteSize > kMaximumBlobBytes ||
                asset.kind == daw::AssetKind::Unknown ||
                !transferKind(asset.kind)) {
                assetFailure = QStringLiteral(
                    "A staged asset identity is incomplete or unsupported");
                return;
            }
            const auto found = groupById.constFind(id);
            if (found == groupById.cend()) {
                groupById.insert(id, groups.size());
                groups.push_back(Group{asset, 1});
                return;
            }
            Group& group = groups[*found];
            if (group.expected.sha256 != asset.sha256 ||
                group.expected.byteSize != asset.byteSize ||
                group.expected.kind != asset.kind) {
                assetFailure = QStringLiteral(
                    "One asset id refers to inconsistent staged content");
                return;
            }
            ++group.references;
        });
    if (!assetFailure.isEmpty()) return reject(assetFailure);

    if (groups.size() != input.assetSources.size()) {
        return reject(QStringLiteral(
            "Every staged asset requires exactly one local source"));
    }
    QHash<QString, CloudPublicationAssetSource> sources;
    for (CloudPublicationAssetSource source : input.assetSources) {
        const QString id = canonicalUuid(source.assetId);
        source.contentType = source.contentType.trimmed();
        if (id.isEmpty() || source.sourcePath.isEmpty() ||
            source.sourcePath.size() > 32768 ||
            source.sourcePath.contains(QChar::Null) ||
            !validContentType(source.contentType) || sources.contains(id)) {
            return reject(QStringLiteral(
                "A local asset source mapping is invalid or duplicated"));
        }
        source.assetId = id;
        sources.insert(id, std::move(source));
    }

    auto prepared = std::make_shared<PreparedPublication>();
    prepared->document = input.project;
    prepared->metadata = input.metadata;
    prepared->metadata.title = prepared->metadata.title.trimmed();
    prepared->metadata.engineVersion =
        prepared->metadata.engineVersion.trimmed();
    prepared->metadata.minimumAppVersion =
        prepared->metadata.minimumAppVersion.trimmed();
    prepared->assets.reserve(groups.size());
    for (const Group& group : groups) {
        const QString id = QString::fromStdString(group.expected.assetId);
        const auto source = sources.constFind(id);
        if (source == sources.cend()) {
            return reject(QStringLiteral(
                "Every staged asset requires exactly one local source"));
        }
        const QFileInfo info(source->sourcePath);
        if (!info.exists() || !info.isFile() || !info.isReadable() ||
            info.size() <= 0 || quint64(info.size()) != group.expected.byteSize) {
            return reject(QStringLiteral(
                "A staged asset source is unavailable or changed size"));
        }
        QFile readable(source->sourcePath);
        if (!readable.open(QIODevice::ReadOnly)) {
            return reject(QStringLiteral(
                "A staged asset source cannot be opened safely"));
        }
        readable.close();

        QString displayName = safeBasename(
            QString::fromStdString(group.expected.originalName));
        if (displayName.isEmpty())
            displayName = safeBasename(source->sourcePath);
        const auto kind = transferKind(group.expected.kind);
        if (displayName.isEmpty() || !kind) {
            return reject(QStringLiteral(
                "A staged asset has unsupported publication metadata"));
        }
        prepared->assets.push_back(PreparedAsset{
            group.expected,
            source->sourcePath,
            source->contentType,
            displayName,
            *kind,
            group.references,
        });
    }

    // The regular V1 inspector is deliberately strict about complete hashes.
    // A capture stage is allowed to leave only sha256 empty, so validate a
    // detached structural copy with an impossible placeholder. No partial
    // document is ever serialized or uploaded.
    daw::ProjectModel structural = prepared->document;
    visitProjectAssets(
        structural, [](daw::AssetRef& asset, const QString&) {
            if (!asset.empty() && asset.sha256.empty())
                asset.sha256 = kPartialHashPlaceholder;
        });
    const daw::cloud::PublishPreflightReport structuralReport =
        daw::cloud::inspectForPublishV1(structural);
    if (!structuralReport.canPublish()) {
        return reject(QStringLiteral(
            "The project contains unsupported plugins, entities, or assets"));
    }

    outcome.accepted = true;
    outcome.publication = std::move(prepared);
    return outcome;
}

struct StagedSnapshot {
    bool accepted = false;
    QString safeError;
    QString path;
    QString sha256;
    quint64 byteSize = 0;
    daw::collab::SharedProjectDocument document;
};

StagedSnapshot stageCanonicalSnapshot(daw::ProjectModel project,
                                      const QString& directory,
                                      const QString& filename) {
    StagedSnapshot staged;
    const daw::cloud::PublishPreflightReport report =
        daw::cloud::inspectForPublishV1(project);
    if (!report.canPublish()) {
        staged.safeError = QStringLiteral(
            "Uploaded assets did not produce a publishable document");
        return staged;
    }
    daw::cloud::CloudDocumentProjection projection =
        daw::cloud::projectForCloudSnapshotV1(project);
    if (!projection.valid() ||
        daw::cloud::containsLocalPathOrUiState(projection.document)) {
        staged.safeError = QStringLiteral(
            "The canonical cloud document could not be projected safely");
        return staged;
    }
    staged.document.project = std::move(projection.document);
    staged.document.confirmedSequence = 0;

    std::string encoded;
    const audio::Result serialized =
        daw::collab::serializeSharedProjectSnapshot(staged.document, encoded);
    if (!serialized || encoded.empty() ||
        encoded.size() > daw::collab::kMaximumSharedProjectSnapshotBytes) {
        staged.safeError = QStringLiteral(
            "The canonical cloud snapshot could not be serialized safely");
        return staged;
    }
    if (directory.isEmpty() || filename.isEmpty() ||
        !QDir().mkpath(directory)) {
        staged.safeError = QStringLiteral(
            "The publication staging directory is unavailable");
        return staged;
    }

    const QByteArray bytes(encoded.data(), qsizetype(encoded.size()));
    staged.sha256 = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    staged.byteSize = quint64(bytes.size());
    staged.path = QDir(directory).filePath(filename);
    QSaveFile file(staged.path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit() ||
        !QFile::setPermissions(staged.path, QFileDevice::ReadOwner |
                                               QFileDevice::WriteOwner)) {
        QFile::remove(staged.path);
        staged.path.clear();
        staged.safeError = QStringLiteral(
            "The canonical cloud snapshot could not be staged");
        return staged;
    }
    staged.accepted = true;
    return staged;
}

bool completeUploadedAsset(const CloudAssetUploadResult& result,
                           const QString& projectId,
                           const QString& uploadId,
                           const PreparedAsset& expected) {
    const daw::AssetRef& asset = result.asset;
    const QString resultId = QString::fromStdString(asset.assetId);
    const QString resultSha = QString::fromStdString(asset.sha256);
    const QString expectedSha = QString::fromStdString(expected.expected.sha256);
    return result.projectId == projectId && result.uploadId == uploadId &&
           canonicalUuid(result.blobId).size() == 36 &&
           canonicalUuid(resultId) == resultId && validSha256(resultSha) &&
           resultId == QString::fromStdString(expected.expected.assetId) &&
           (expectedSha.isEmpty() || resultSha == expectedSha) &&
           asset.byteSize == expected.expected.byteSize &&
           asset.kind == expected.expected.kind &&
           result.contentType == expected.contentType &&
           QString::fromStdString(asset.mimeType) == expected.contentType &&
           safeBasename(QString::fromStdString(asset.originalName)) ==
               QString::fromStdString(asset.originalName);
}

int replaceAssetReferences(daw::ProjectModel& document,
                           const PreparedAsset& expected,
                           const daw::AssetRef& uploaded) {
    int replacements = 0;
    bool mismatch = false;
    visitProjectAssets(
        document,
        [&](daw::AssetRef& current, const QString&) {
            if (current.assetId != expected.expected.assetId) return;
            if (current.sha256 != expected.expected.sha256 ||
                current.byteSize != expected.expected.byteSize ||
                current.kind != expected.expected.kind) {
                mismatch = true;
                return;
            }
            daw::AssetRef replacement = uploaded;
            // Audio analysis belongs to the musical reference and is not part
            // of the generic upload response. Preserve it while replacing all
            // server-authoritative identity/name/MIME fields.
            replacement.codec = current.codec;
            replacement.sampleRate = current.sampleRate;
            replacement.channels = current.channels;
            replacement.frames = current.frames;
            current = std::move(replacement);
            ++replacements;
        });
    return mismatch ? -1 : replacements;
}

bool isActivePhase(CloudPublicationPhase phase) {
    switch (phase) {
        case CloudPublicationPhase::Preflight:
        case CloudPublicationPhase::CreatingProject:
        case CloudPublicationPhase::UploadingAssets:
        case CloudPublicationPhase::PreparingSnapshot:
        case CloudPublicationPhase::UploadingSnapshot:
        case CloudPublicationPhase::Activating: return true;
        case CloudPublicationPhase::Idle:
        case CloudPublicationPhase::Completed:
        case CloudPublicationPhase::Failed:
        case CloudPublicationPhase::Cancelled: return false;
    }
    return false;
}

void removeFileOnWorker(const QString& path) {
    if (path.isEmpty()) return;
    QThreadPool::globalInstance()->start([path] { QFile::remove(path); });
}

} // namespace

struct CloudProjectPublisher::Impl {
    struct Ports {
        std::function<quint64(const CreateCloudProjectInput&)> createProject;
        std::function<quint64(const QString&)> publishProject;
        std::function<bool(quint64)> cancelProjectRequest;
        std::function<quint64(const CloudAssetUploadInput&)> uploadAsset;
        std::function<quint64(const CloudSnapshotUploadInput&)> uploadSnapshot;
        std::function<bool(quint64)> retryTransfer;
        std::function<bool(quint64)> cancelTransfer;
        std::function<quint64(const QString&, const QString&)> abortUpload;
        std::function<void(std::function<void()>)> dispatchWorker;
        std::function<QString()> stagingDirectory;
        std::function<QString()> makeUuid;
        std::function<StagedSnapshot(daw::ProjectModel, const QString&,
                                     const QString&)>
            stageSnapshot;
        std::function<void(const QString&)> removeStagedFile;
    } ports;

    struct ActiveAsset {
        int preparedIndex = -1;
        QString uploadId;
        int attempts = 1;
        bool waitingRetry = false;
        quint64 generation = 0;
    };

    CloudProjectPublisher* q = nullptr;
    QPointer<CloudProjectClient> projects;
    QPointer<CloudAssetTransferManager> transfers;
    CloudPublicationPhase phase = CloudPublicationPhase::Idle;
    quint64 generation = 0;
    quint64 generationCounter = 0;
    int maximumConcurrent = kDefaultMaximumConcurrentUploads;
    int completedUnits = 0;
    int totalUnits = 0;
    QString projectId;
    std::shared_ptr<PreparedPublication> prepared;
    QQueue<int> queuedAssets;
    QHash<quint64, ActiveAsset> activeAssets;
    quint64 createRequestId = 0;
    quint64 publishRequestId = 0;
    quint64 snapshotTransferId = 0;
    QString snapshotUploadId;
    QString snapshotPath;
    QString snapshotSha256;
    quint64 snapshotByteSize = 0;
    QStringList snapshotAssetIds;
    int snapshotAttempts = 0;
    bool snapshotWaitingRetry = false;
    CloudSnapshotUploadResult snapshotResult;
    daw::collab::SharedProjectDocument canonicalDocument;

    explicit Impl(CloudProjectPublisher* owner) : q(owner) {}

    void setPhase(CloudPublicationPhase next) {
        if (phase == next) return;
        phase = next;
        emit q->phaseChanged(generation, phase);
    }

    void emitProgress() {
        emit q->progressChanged(generation, completedUnits, totalUnits);
    }

    void cleanupSnapshotPath() {
        const QString path = std::exchange(snapshotPath, {});
        if (!path.isEmpty() && ports.removeStagedFile)
            ports.removeStagedFile(path);
    }

    void cancelOperations() {
        if (createRequestId && ports.cancelProjectRequest)
            ports.cancelProjectRequest(createRequestId);
        if (publishRequestId && ports.cancelProjectRequest)
            ports.cancelProjectRequest(publishRequestId);
        createRequestId = 0;
        publishRequestId = 0;
        if (ports.cancelTransfer) {
            for (auto iterator = activeAssets.cbegin();
                 iterator != activeAssets.cend(); ++iterator) {
                ports.cancelTransfer(iterator.key());
            }
            if (snapshotTransferId)
                ports.cancelTransfer(snapshotTransferId);
        }
        if (ports.abortUpload && !projectId.isEmpty()) {
            for (auto iterator = activeAssets.cbegin();
                 iterator != activeAssets.cend(); ++iterator) {
                if (!iterator->uploadId.isEmpty())
                    ports.abortUpload(projectId, iterator->uploadId);
            }
            if (!snapshotUploadId.isEmpty() && snapshotTransferId)
                ports.abortUpload(projectId, snapshotUploadId);
        }
        activeAssets.clear();
        queuedAssets.clear();
        snapshotTransferId = 0;
        snapshotWaitingRetry = false;
        cleanupSnapshotPath();
    }

    void clearPublicationData() {
        prepared.reset();
        snapshotUploadId.clear();
        snapshotSha256.clear();
        snapshotByteSize = 0;
        snapshotAssetIds.clear();
        snapshotAttempts = 0;
        snapshotResult = {};
        canonicalDocument = {};
    }

    void cancelCurrent(bool notify) {
        if (!isActivePhase(phase)) {
            cancelOperations();
            clearPublicationData();
            return;
        }
        const quint64 oldGeneration = generation;
        const QString oldProjectId = projectId;
        cancelOperations();
        clearPublicationData();
        if (notify) {
            setPhase(CloudPublicationPhase::Cancelled);
            emit q->publicationCancelled(oldGeneration, oldProjectId);
        }
    }

    void fail(CloudPublicationPhase failedPhase, const QString& message,
              bool retryable) {
        if (!isActivePhase(phase) || failedPhase != phase) return;
        const QString retainedProjectId = projectId;
        cancelOperations();
        setPhase(CloudPublicationPhase::Failed);
        emit q->publicationFailed(
            generation, failedPhase, retainedProjectId,
            message.isEmpty() ? QStringLiteral("Cloud publication failed")
                              : message,
            retryable);
    }

    void dispatchPreflight(CloudProjectPublicationInput input) {
        const quint64 taskGeneration = generation;
        QPointer<CloudProjectPublisher> guard(q);
        ports.dispatchWorker(
            [guard, taskGeneration, input = std::move(input)]() mutable {
                PreflightOutcome outcome = performPreflight(std::move(input));
                if (!guard) return;
                auto deliver =
                    [guard, taskGeneration,
                     outcome = std::move(outcome)]() mutable {
                        if (!guard || !guard->m_impl) return;
                        guard->m_impl->preflightFinished(
                            taskGeneration, std::move(outcome));
                    };
                if (QThread::currentThread() == guard->thread())
                    deliver();
                else
                    QMetaObject::invokeMethod(guard, std::move(deliver),
                                              Qt::QueuedConnection);
            });
    }

    void preflightFinished(quint64 taskGeneration,
                           PreflightOutcome outcome) {
        if (taskGeneration != generation ||
            phase != CloudPublicationPhase::Preflight)
            return;
        if (!outcome.accepted || !outcome.publication) {
            fail(CloudPublicationPhase::Preflight, outcome.safeError, false);
            return;
        }
        prepared = std::move(outcome.publication);
        totalUnits = prepared->assets.size() + 3;
        emitProgress();
        setPhase(CloudPublicationPhase::CreatingProject);
        if (!ports.createProject) {
            fail(CloudPublicationPhase::CreatingProject,
                 QStringLiteral("Cloud project service is unavailable"), true);
            return;
        }
        createRequestId = ports.createProject(prepared->metadata);
        if (!createRequestId) {
            fail(CloudPublicationPhase::CreatingProject,
                 QStringLiteral("Cloud project creation could not start"),
                 true);
        }
    }

    bool validCreatedProject(const CloudProjectView& view) const {
        return prepared && canonicalUuid(view.project.id) == view.project.id &&
               view.project.status == CloudProjectStatus::Uploading &&
               view.role == CloudProjectRole::Owner &&
               view.project.title == prepared->metadata.title &&
               view.project.engineVersion == prepared->metadata.engineVersion &&
               view.project.minimumAppVersion ==
                   prepared->metadata.minimumAppVersion &&
               view.project.formatVersion ==
                   daw::ProjectSerializer::kFormatVersion &&
               view.project.headSequence == 0 &&
               view.project.snapshotSequence == 0;
    }

    void projectReceived(quint64 requestId, CloudRequestKind kind,
                         const CloudProjectView& view) {
        if (kind == CloudRequestKind::CreateProject &&
            phase == CloudPublicationPhase::CreatingProject &&
            requestId == createRequestId) {
            createRequestId = 0;
            if (!validCreatedProject(view)) {
                fail(CloudPublicationPhase::CreatingProject,
                     QStringLiteral("Created cloud project metadata mismatched"),
                     false);
                return;
            }
            projectId = view.project.id;
            ++completedUnits;
            emitProgress();
            queuedAssets.clear();
            for (int index = 0; index < prepared->assets.size(); ++index)
                queuedAssets.enqueue(index);
            setPhase(CloudPublicationPhase::UploadingAssets);
            pumpAssets();
            return;
        }
        if (kind == CloudRequestKind::PublishProject &&
            phase == CloudPublicationPhase::Activating &&
            requestId == publishRequestId) {
            publishRequestId = 0;
            const bool accepted = prepared && view.project.id == projectId &&
                                  view.project.status ==
                                      CloudProjectStatus::Active &&
                                  view.role == CloudProjectRole::Owner &&
                                  view.project.formatVersion ==
                                      daw::ProjectSerializer::kFormatVersion &&
                                  view.project.headSequence == 0 &&
                                  view.project.snapshotSequence == 0;
            if (!accepted) {
                fail(CloudPublicationPhase::Activating,
                     QStringLiteral("Activated cloud project metadata mismatched"),
                     false);
                return;
            }
            ++completedUnits;
            emitProgress();
            CloudProjectPublicationResult result;
            result.project = view;
            result.snapshot = snapshotResult;
            result.canonicalDocument = canonicalDocument;
            cleanupSnapshotPath();
            setPhase(CloudPublicationPhase::Completed);
            emit q->publicationCompleted(generation, result);
        }
    }

    void requestFailed(quint64 requestId, CloudRequestKind kind,
                       const CloudClientError& error) {
        if (kind == CloudRequestKind::CreateProject &&
            phase == CloudPublicationPhase::CreatingProject &&
            requestId == createRequestId) {
            createRequestId = 0;
            fail(CloudPublicationPhase::CreatingProject, error.safeMessage,
                 error.retryable);
        } else if (kind == CloudRequestKind::PublishProject &&
                   phase == CloudPublicationPhase::Activating &&
                   requestId == publishRequestId) {
            publishRequestId = 0;
            fail(CloudPublicationPhase::Activating, error.safeMessage,
                 error.retryable);
        }
    }

    void pumpAssets() {
        if (phase != CloudPublicationPhase::UploadingAssets || !prepared)
            return;
        while (!queuedAssets.isEmpty() &&
               activeAssets.size() < maximumConcurrent) {
            const int index = queuedAssets.dequeue();
            if (index < 0 || index >= prepared->assets.size()) {
                fail(CloudPublicationPhase::UploadingAssets,
                     QStringLiteral("Publication asset queue is inconsistent"),
                     false);
                return;
            }
            const PreparedAsset& item = prepared->assets[index];
            const QString uploadId = ports.makeUuid ? ports.makeUuid() : QString();
            if (canonicalUuid(uploadId).isEmpty() || !ports.uploadAsset) {
                fail(CloudPublicationPhase::UploadingAssets,
                     QStringLiteral("Asset upload could not be initialized"),
                     true);
                return;
            }
            CloudAssetUploadInput input;
            input.projectId = projectId;
            input.uploadId = uploadId;
            input.assetId = QString::fromStdString(item.expected.assetId);
            input.sourcePath = item.sourcePath;
            input.sha256 = QString::fromStdString(item.expected.sha256);
            input.byteSize = item.expected.byteSize;
            input.kind = item.kind;
            input.contentType = item.contentType;
            input.displayName = item.displayName;
            const quint64 transferId = ports.uploadAsset(input);
            if (!transferId) {
                fail(CloudPublicationPhase::UploadingAssets,
                     QStringLiteral("Asset upload could not start"), true);
                return;
            }
            activeAssets.insert(
                transferId,
                ActiveAsset{index, uploadId, 1, false, generation});
        }
        if (queuedAssets.isEmpty() && activeAssets.isEmpty())
            prepareSnapshot();
    }

    void assetCompleted(quint64 transferId,
                        const CloudAssetUploadResult& result) {
        if (phase != CloudPublicationPhase::UploadingAssets || !prepared)
            return;
        const auto iterator = activeAssets.find(transferId);
        if (iterator == activeAssets.end() ||
            iterator->generation != generation)
            return;
        const ActiveAsset active = iterator.value();
        if (active.preparedIndex < 0 ||
            active.preparedIndex >= prepared->assets.size()) {
            fail(CloudPublicationPhase::UploadingAssets,
                 QStringLiteral("Completed asset was not expected"), false);
            return;
        }
        const PreparedAsset& expected =
            prepared->assets[active.preparedIndex];
        if (!completeUploadedAsset(result, projectId, active.uploadId,
                                   expected)) {
            fail(CloudPublicationPhase::UploadingAssets,
                 QStringLiteral("Completed asset metadata mismatched"), false);
            return;
        }
        const int replaced = replaceAssetReferences(
            prepared->document, expected, result.asset);
        if (replaced != expected.referenceCount) {
            fail(CloudPublicationPhase::UploadingAssets,
                 QStringLiteral("Completed asset references changed during publication"),
                 false);
            return;
        }
        activeAssets.erase(iterator);
        ++completedUnits;
        emitProgress();
        pumpAssets();
    }

    void transferFailed(quint64 transferId, CloudTransferKind kind,
                        const CloudTransferError& error) {
        if (kind == CloudTransferKind::AssetUpload &&
            phase == CloudPublicationPhase::UploadingAssets) {
            auto iterator = activeAssets.find(transferId);
            if (iterator == activeAssets.end() ||
                iterator->generation != generation)
                return;
            if (error.retryable &&
                iterator->attempts < kMaximumTransferAttempts &&
                !iterator->waitingRetry && ports.retryTransfer) {
                iterator->waitingRetry = true;
                ++iterator->attempts;
                const quint64 taskGeneration = generation;
                QPointer<CloudProjectPublisher> guard(q);
                const int delay = 250 * (1 << (iterator->attempts - 2));
                QTimer::singleShot(
                    delay, q, [guard, taskGeneration, transferId] {
                        if (!guard || !guard->m_impl) return;
                        guard->m_impl->retryAsset(taskGeneration, transferId);
                    });
                return;
            }
            fail(CloudPublicationPhase::UploadingAssets, error.safeMessage,
                 error.retryable);
            return;
        }
        if (kind == CloudTransferKind::SnapshotUpload &&
            phase == CloudPublicationPhase::UploadingSnapshot &&
            transferId == snapshotTransferId) {
            if (error.retryable &&
                snapshotAttempts < kMaximumTransferAttempts &&
                !snapshotWaitingRetry && ports.retryTransfer) {
                snapshotWaitingRetry = true;
                ++snapshotAttempts;
                const quint64 taskGeneration = generation;
                QPointer<CloudProjectPublisher> guard(q);
                const int delay = 250 * (1 << (snapshotAttempts - 2));
                QTimer::singleShot(
                    delay, q, [guard, taskGeneration, transferId] {
                        if (!guard || !guard->m_impl) return;
                        guard->m_impl->retrySnapshot(taskGeneration,
                                                     transferId);
                    });
                return;
            }
            fail(CloudPublicationPhase::UploadingSnapshot, error.safeMessage,
                 error.retryable);
        }
    }

    void retryAsset(quint64 taskGeneration, quint64 transferId) {
        if (taskGeneration != generation ||
            phase != CloudPublicationPhase::UploadingAssets)
            return;
        auto iterator = activeAssets.find(transferId);
        if (iterator == activeAssets.end() || !iterator->waitingRetry) return;
        iterator->waitingRetry = false;
        if (!ports.retryTransfer || !ports.retryTransfer(transferId)) {
            fail(CloudPublicationPhase::UploadingAssets,
                 QStringLiteral("Asset upload retry could not start"), true);
        }
    }

    void retrySnapshot(quint64 taskGeneration, quint64 transferId) {
        if (taskGeneration != generation ||
            phase != CloudPublicationPhase::UploadingSnapshot ||
            transferId != snapshotTransferId || !snapshotWaitingRetry)
            return;
        snapshotWaitingRetry = false;
        if (!ports.retryTransfer || !ports.retryTransfer(transferId)) {
            fail(CloudPublicationPhase::UploadingSnapshot,
                 QStringLiteral("Snapshot upload retry could not start"), true);
        }
    }

    void prepareSnapshot() {
        if (phase != CloudPublicationPhase::UploadingAssets || !prepared ||
            !queuedAssets.isEmpty() || !activeAssets.isEmpty())
            return;
        setPhase(CloudPublicationPhase::PreparingSnapshot);
        const quint64 taskGeneration = generation;
        const QString uploadId = ports.makeUuid ? ports.makeUuid() : QString();
        const QString directory =
            ports.stagingDirectory ? ports.stagingDirectory() : QString();
        const QString filename = uploadId + QStringLiteral(".json");
        if (canonicalUuid(uploadId).isEmpty() || !ports.stageSnapshot) {
            fail(CloudPublicationPhase::PreparingSnapshot,
                 QStringLiteral("Snapshot staging could not be initialized"),
                 false);
            return;
        }
        snapshotUploadId = uploadId;
        daw::ProjectModel document = prepared->document;
        const auto stage = ports.stageSnapshot;
        QPointer<CloudProjectPublisher> guard(q);
        ports.dispatchWorker(
            [guard, taskGeneration, document = std::move(document), directory,
             filename, stage]() mutable {
                StagedSnapshot staged =
                    stage(std::move(document), directory, filename);
                if (!guard) {
                    if (!staged.path.isEmpty()) QFile::remove(staged.path);
                    return;
                }
                auto deliver =
                    [guard, taskGeneration,
                     staged = std::move(staged)]() mutable {
                        if (!guard || !guard->m_impl) {
                            if (!staged.path.isEmpty())
                                removeFileOnWorker(staged.path);
                            return;
                        }
                        guard->m_impl->snapshotPrepared(
                            taskGeneration, std::move(staged));
                    };
                if (QThread::currentThread() == guard->thread())
                    deliver();
                else
                    QMetaObject::invokeMethod(guard, std::move(deliver),
                                              Qt::QueuedConnection);
            });
    }

    void snapshotPrepared(quint64 taskGeneration, StagedSnapshot staged) {
        if (taskGeneration != generation ||
            phase != CloudPublicationPhase::PreparingSnapshot) {
            if (!staged.path.isEmpty() && ports.removeStagedFile)
                ports.removeStagedFile(staged.path);
            return;
        }
        const CloudSnapshotAssetManifest manifest = staged.accepted
            ? collectCloudSnapshotAssetManifest(staged.document.project)
            : CloudSnapshotAssetManifest{};
        if (!staged.accepted || !manifest.accepted || staged.path.isEmpty() ||
            !validSha256(staged.sha256) || staged.byteSize == 0 ||
            staged.document.confirmedSequence != 0) {
            if (!staged.path.isEmpty() && ports.removeStagedFile)
                ports.removeStagedFile(staged.path);
            fail(CloudPublicationPhase::PreparingSnapshot,
                 staged.safeError.isEmpty() ? manifest.safeError
                                            : staged.safeError,
                 false);
            return;
        }
        snapshotPath = std::move(staged.path);
        snapshotSha256 = std::move(staged.sha256);
        snapshotByteSize = staged.byteSize;
        snapshotAssetIds = manifest.assetIds;
        canonicalDocument = std::move(staged.document);
        setPhase(CloudPublicationPhase::UploadingSnapshot);
        if (!ports.uploadSnapshot) {
            fail(CloudPublicationPhase::UploadingSnapshot,
                 QStringLiteral("Snapshot upload service is unavailable"),
                 true);
            return;
        }
        CloudSnapshotUploadInput input;
        input.projectId = projectId;
        input.uploadId = snapshotUploadId;
        input.sourcePath = snapshotPath;
        input.sequence = 0;
        input.schemaVersion = daw::ProjectSerializer::kFormatVersion;
        input.sha256 = snapshotSha256;
        input.byteSize = snapshotByteSize;
        input.assetIds = snapshotAssetIds;
        snapshotTransferId = ports.uploadSnapshot(input);
        snapshotAttempts = 1;
        if (!snapshotTransferId) {
            fail(CloudPublicationPhase::UploadingSnapshot,
                 QStringLiteral("Snapshot upload could not start"), true);
        }
    }

    void snapshotCompleted(quint64 transferId,
                           const CloudSnapshotUploadResult& result) {
        if (phase != CloudPublicationPhase::UploadingSnapshot ||
            transferId != snapshotTransferId)
            return;
        const bool accepted = result.projectId == projectId &&
                              result.uploadId == snapshotUploadId &&
                              canonicalUuid(result.snapshotId) ==
                                  result.snapshotId &&
                              canonicalUuid(result.blobId) == result.blobId &&
                              result.sequence == 0 &&
                              result.schemaVersion ==
                                  daw::ProjectSerializer::kFormatVersion &&
                              result.sha256 == snapshotSha256 &&
                              result.byteSize == snapshotByteSize &&
                              result.assetIds == snapshotAssetIds &&
                              result.contentType == QStringLiteral(
                                  "application/vnd.vlt.project+json");
        if (!accepted) {
            fail(CloudPublicationPhase::UploadingSnapshot,
                 QStringLiteral("Completed snapshot metadata mismatched"),
                 false);
            return;
        }
        snapshotTransferId = 0;
        snapshotResult = result;
        ++completedUnits;
        emitProgress();
        setPhase(CloudPublicationPhase::Activating);
        if (!ports.publishProject) {
            fail(CloudPublicationPhase::Activating,
                 QStringLiteral("Cloud activation service is unavailable"),
                 true);
            return;
        }
        publishRequestId = ports.publishProject(projectId);
        if (!publishRequestId) {
            fail(CloudPublicationPhase::Activating,
                 QStringLiteral("Cloud activation could not start"), true);
        }
    }
};

CloudProjectPublisher::CloudProjectPublisher(
    CloudProjectClient* projects, CloudAssetTransferManager* transfers,
    QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(this)) {
    m_impl->projects = projects;
    m_impl->transfers = transfers;
    m_impl->ports.createProject =
        [guard = QPointer<CloudProjectClient>(projects)](
            const CreateCloudProjectInput& input) {
            return guard ? guard->createProject(input) : quint64(0);
        };
    m_impl->ports.publishProject =
        [guard = QPointer<CloudProjectClient>(projects)](
            const QString& projectId) {
            return guard ? guard->publishProject(projectId) : quint64(0);
        };
    m_impl->ports.cancelProjectRequest =
        [guard = QPointer<CloudProjectClient>(projects)](quint64 requestId) {
            return guard && guard->cancel(requestId);
        };
    m_impl->ports.uploadAsset =
        [guard = QPointer<CloudAssetTransferManager>(transfers)](
            const CloudAssetUploadInput& input) {
            return guard ? guard->uploadAsset(input) : quint64(0);
        };
    m_impl->ports.uploadSnapshot =
        [guard = QPointer<CloudAssetTransferManager>(transfers)](
            const CloudSnapshotUploadInput& input) {
            return guard ? guard->uploadSnapshot(input) : quint64(0);
        };
    m_impl->ports.retryTransfer =
        [guard = QPointer<CloudAssetTransferManager>(transfers)](
            quint64 transferId) {
            return guard && guard->retry(transferId);
        };
    m_impl->ports.cancelTransfer =
        [guard = QPointer<CloudAssetTransferManager>(transfers)](
            quint64 transferId) {
            return guard && guard->cancel(transferId);
        };
    m_impl->ports.abortUpload =
        [guard = QPointer<CloudAssetTransferManager>(transfers)](
            const QString& projectId, const QString& uploadId) {
            return guard ? guard->abortUpload(projectId, uploadId)
                         : quint64(0);
        };
    m_impl->ports.dispatchWorker = [](std::function<void()> task) {
        QThreadPool::globalInstance()->start(std::move(task));
    };
    m_impl->ports.stagingDirectory = [] {
        return QDir(QStandardPaths::writableLocation(
                        QStandardPaths::CacheLocation))
            .filePath(QStringLiteral(
                "collaboration/project-publish-staging-v1"));
    };
    m_impl->ports.makeUuid = [] {
        return QUuid::createUuid()
            .toString(QUuid::WithoutBraces)
            .toLower();
    };
    m_impl->ports.stageSnapshot = stageCanonicalSnapshot;
    m_impl->ports.removeStagedFile = removeFileOnWorker;

    if (projects) {
        connect(projects, &CloudProjectClient::projectReceived, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudProjectView& project) {
                    if (m_impl)
                        m_impl->projectReceived(requestId, kind, project);
                });
        connect(projects, &CloudProjectClient::requestFailed, this,
                [this](quint64 requestId, CloudRequestKind kind,
                       const CloudClientError& error) {
                    if (m_impl)
                        m_impl->requestFailed(requestId, kind, error);
                });
    }
    if (transfers) {
        connect(transfers,
                &CloudAssetTransferManager::assetUploadCompleted, this,
                [this](quint64 transferId,
                       const CloudAssetUploadResult& result) {
                    if (m_impl)
                        m_impl->assetCompleted(transferId, result);
                });
        connect(transfers,
                &CloudAssetTransferManager::snapshotUploadCompleted, this,
                [this](quint64 transferId,
                       const CloudSnapshotUploadResult& result) {
                    if (m_impl)
                        m_impl->snapshotCompleted(transferId, result);
                });
        connect(transfers, &CloudAssetTransferManager::transferFailed, this,
                [this](quint64 transferId, CloudTransferKind kind,
                       const CloudTransferError& error) {
                    if (m_impl)
                        m_impl->transferFailed(transferId, kind, error);
                });
    }
    qRegisterMetaType<CloudPublicationPhase>();
    qRegisterMetaType<CloudProjectPublicationResult>();
}

CloudProjectPublisher::~CloudProjectPublisher() {
    if (m_impl) m_impl->cancelCurrent(false);
}

quint64 CloudProjectPublisher::publish(
    const CloudProjectPublicationInput& input) {
    if (!m_impl) return 0;
    m_impl->cancelCurrent(true);
    ++m_impl->generationCounter;
    if (!m_impl->generationCounter) ++m_impl->generationCounter;
    m_impl->generation = m_impl->generationCounter;
    m_impl->projectId.clear();
    m_impl->completedUnits = 0;
    m_impl->totalUnits =
        input.assetSources.size() <=
                std::numeric_limits<int>::max() - 3
            ? input.assetSources.size() + 3
            : 3;
    m_impl->clearPublicationData();
    m_impl->setPhase(CloudPublicationPhase::Preflight);
    m_impl->emitProgress();
    m_impl->dispatchPreflight(input);
    return m_impl->generation;
}

void CloudProjectPublisher::cancel() {
    if (m_impl) m_impl->cancelCurrent(true);
}

CloudPublicationPhase CloudProjectPublisher::phase() const noexcept {
    return m_impl ? m_impl->phase : CloudPublicationPhase::Idle;
}

quint64 CloudProjectPublisher::generation() const noexcept {
    return m_impl ? m_impl->generation : 0;
}

QString CloudProjectPublisher::cloudProjectId() const {
    return m_impl ? m_impl->projectId : QString();
}

void CloudProjectPublisher::setMaximumConcurrentAssetUploads(int maximum) {
    if (!m_impl) return;
    m_impl->maximumConcurrent =
        std::clamp(maximum, 1, kMaximumConcurrentUploads);
    m_impl->pumpAssets();
}

int CloudProjectPublisher::maximumConcurrentAssetUploads() const noexcept {
    return m_impl ? m_impl->maximumConcurrent
                  : kDefaultMaximumConcurrentUploads;
}

bool checkCloudProjectPublisherForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    QTemporaryDir directory;
    if (!directory.isValid())
        return fail(QStringLiteral("publisher test directory is unavailable"));

    const QByteArray clipBytes("RIFF-publisher-clip");
    const QByteArray sampleBytes("RIFF-publisher-sampler");
    const QString clipPath =
        directory.filePath(QStringLiteral("private-clip.wav"));
    const QString samplePath =
        directory.filePath(QStringLiteral("private-sample.wav"));
    const auto writeFixture = [](const QString& path,
                                 const QByteArray& bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) &&
               file.write(bytes) == bytes.size() && file.flush();
    };
    if (!writeFixture(clipPath, clipBytes) ||
        !writeFixture(samplePath, sampleBytes)) {
        return fail(QStringLiteral("publisher asset fixture could not be written"));
    }

    const auto partialAsset = [](const char* id, quint64 bytes,
                                 const char* name) {
        daw::AssetRef result;
        result.assetId = id;
        result.kind = daw::AssetKind::Audio;
        result.byteSize = bytes;
        result.originalName = name;
        result.mimeType = "audio/wav";
        return result;
    };
    const auto builtin = [](const char* id, const char* uid) {
        daw::InsertModel insert;
        insert.id = id;
        insert.name = uid;
        insert.format = daw::PluginFormat::Internal;
        insert.uid = uid;
        insert.path = "/Applications/VLT/internal";
        insert.pluginVersion = "1.0";
        insert.stateSchemaVersion = 1;
        return insert;
    };

    const QString clipAssetId =
        QStringLiteral("71000000-0000-4000-8000-000000000001");
    const QString sampleAssetId =
        QStringLiteral("71000000-0000-4000-8000-000000000002");
    daw::ProjectModel source;
    source.loopEnabled = true;
    source.loopStartSeconds = 1.0;
    source.loopEndSeconds = 2.0;
    source.masterInserts.push_back(
        builtin("72000000-0000-4000-8000-000000000001",
                "daw.equalizer"));
    daw::TrackModel track;
    track.id = "73000000-0000-4000-8000-000000000001";
    track.armed = true;
    track.instrument =
        builtin("74000000-0000-4000-8000-000000000001", "daw.sampler");
    track.instrument.assetBindings.push_back(daw::PluginAssetBinding{
        "sample",
        partialAsset(sampleAssetId.toUtf8().constData(), sampleBytes.size(),
                     "/Users/local/private-sample.wav"),
        true,
    });
    daw::ClipModel clip;
    clip.id = "75000000-0000-4000-8000-000000000001";
    clip.kind = daw::ClipKind::Audio;
    clip.filePath = clipPath.toStdString();
    clip.asset = partialAsset(clipAssetId.toUtf8().constData(),
                              clipBytes.size(),
                              "/Users/local/private-clip.wav");
    clip.asset.codec = "pcm_s16le";
    clip.asset.sampleRate = 48000.0;
    clip.asset.channels = 2;
    clip.asset.frames = 128;
    track.clips.push_back(std::move(clip));
    source.tracks.push_back(std::move(track));

    CloudProjectPublicationInput input;
    input.project = source;
    input.metadata.title = QStringLiteral("Publisher test");
    input.metadata.engineVersion = QStringLiteral("test-engine");
    input.metadata.minimumAppVersion = QStringLiteral("0.1.0");
    input.assetSources = {
        {clipAssetId, clipPath, QStringLiteral("audio/wav")},
        {sampleAssetId, samplePath, QStringLiteral("audio/wav")},
    };

    CloudProjectPublisher publisher(nullptr, nullptr);
    publisher.setMaximumConcurrentAssetUploads(1);
    quint64 nextRequestId = 100;
    quint64 nextTransferId = 200;
    int uuidCounter = 1;
    QVector<QPair<quint64, CloudAssetUploadInput>> assetUploads;
    QVector<QPair<quint64, CloudSnapshotUploadInput>> snapshotUploads;
    QVector<CreateCloudProjectInput> creates;
    QVector<QString> activations;
    QVector<quint64> cancelledRequests;
    QVector<quint64> cancelledTransfers;
    QVector<QPair<QString, QString>> abortedUploads;
    QVector<quint64> retriedTransfers;
    publisher.m_impl->ports.createProject =
        [&](const CreateCloudProjectInput& metadata) {
            creates.push_back(metadata);
            return ++nextRequestId;
        };
    publisher.m_impl->ports.publishProject = [&](const QString& projectId) {
        activations.push_back(projectId);
        return ++nextRequestId;
    };
    publisher.m_impl->ports.cancelProjectRequest = [&](quint64 requestId) {
        cancelledRequests.push_back(requestId);
        return true;
    };
    publisher.m_impl->ports.uploadAsset =
        [&](const CloudAssetUploadInput& upload) {
            const quint64 id = ++nextTransferId;
            assetUploads.push_back({id, upload});
            return id;
        };
    publisher.m_impl->ports.uploadSnapshot =
        [&](const CloudSnapshotUploadInput& upload) {
            const quint64 id = ++nextTransferId;
            snapshotUploads.push_back({id, upload});
            return id;
        };
    publisher.m_impl->ports.retryTransfer = [&](quint64 transferId) {
        retriedTransfers.push_back(transferId);
        return true;
    };
    publisher.m_impl->ports.cancelTransfer = [&](quint64 transferId) {
        cancelledTransfers.push_back(transferId);
        return true;
    };
    publisher.m_impl->ports.abortUpload =
        [&](const QString& projectId, const QString& uploadId) {
            abortedUploads.push_back({projectId, uploadId});
            return ++nextTransferId;
        };
    publisher.m_impl->ports.dispatchWorker =
        [](std::function<void()> task) { task(); };
    publisher.m_impl->ports.stagingDirectory = [&] { return directory.path(); };
    publisher.m_impl->ports.makeUuid = [&] {
        return QStringLiteral("a0000000-0000-4000-8000-%1")
            .arg(uuidCounter++, 12, 10, QLatin1Char('0'));
    };
    publisher.m_impl->ports.stageSnapshot = stageCanonicalSnapshot;
    publisher.m_impl->ports.removeStagedFile =
        [](const QString& path) { QFile::remove(path); };

    QVector<CloudPublicationPhase> phases;
    QVector<QPair<int, int>> progress;
    bool completed = false;
    CloudProjectPublicationResult completedResult;
    QObject::connect(
        &publisher, &CloudProjectPublisher::phaseChanged, &publisher,
        [&](quint64, CloudPublicationPhase phase) { phases.push_back(phase); });
    QObject::connect(
        &publisher, &CloudProjectPublisher::progressChanged, &publisher,
        [&](quint64, int done, int total) {
            progress.push_back({done, total});
        });
    QObject::connect(
        &publisher, &CloudProjectPublisher::publicationCompleted, &publisher,
        [&](quint64, const CloudProjectPublicationResult& result) {
            completed = true;
            completedResult = result;
        });

    CloudProjectPublicationInput missingSource = input;
    missingSource.assetSources.removeLast();
    publisher.publish(missingSource);
    if (publisher.phase() != CloudPublicationPhase::Failed ||
        !creates.isEmpty()) {
        return fail(QStringLiteral(
            "publisher created a project before strict source preflight"));
    }

    phases.clear();
    progress.clear();
    const quint64 generation = publisher.publish(input);
    if (!generation || creates.size() != 1 ||
        publisher.phase() != CloudPublicationPhase::CreatingProject) {
        return fail(QStringLiteral("publisher did not finish structural preflight"));
    }

    const QString projectId =
        QStringLiteral("76000000-0000-4000-8000-000000000001");
    CloudProjectView uploading;
    uploading.role = CloudProjectRole::Owner;
    uploading.project.id = projectId;
    uploading.project.title = input.metadata.title;
    uploading.project.engineVersion = input.metadata.engineVersion;
    uploading.project.minimumAppVersion = input.metadata.minimumAppVersion;
    uploading.project.formatVersion = daw::ProjectSerializer::kFormatVersion;
    uploading.project.status = CloudProjectStatus::Uploading;
    publisher.m_impl->projectReceived(
        publisher.m_impl->createRequestId, CloudRequestKind::CreateProject,
        uploading);
    if (assetUploads.size() != 1 ||
        publisher.phase() != CloudPublicationPhase::UploadingAssets) {
        return fail(QStringLiteral("publisher did not enforce upload concurrency"));
    }
    CloudTransferError retryableAssetError;
    retryableAssetError.safeMessage = QStringLiteral("temporary upload failure");
    retryableAssetError.retryable = true;
    publisher.m_impl->transferFailed(assetUploads.front().first,
                                     CloudTransferKind::AssetUpload,
                                     retryableAssetError);
    publisher.m_impl->retryAsset(generation, assetUploads.front().first);
    if (retriedTransfers != QVector<quint64>{assetUploads.front().first} ||
        publisher.phase() != CloudPublicationPhase::UploadingAssets) {
        return fail(QStringLiteral("publisher did not resume a retryable upload"));
    }

    const auto completeAsset = [&](int uploadIndex,
                                   const QByteArray& bytes) {
        const auto upload = assetUploads.at(uploadIndex);
        CloudAssetUploadResult result;
        result.projectId = upload.second.projectId;
        result.uploadId = upload.second.uploadId;
        result.blobId =
            QStringLiteral("b0000000-0000-4000-8000-%1")
                .arg(uploadIndex + 1, 12, 10, QLatin1Char('0'));
        result.contentType = upload.second.contentType;
        result.asset.assetId = upload.second.assetId.toStdString();
        result.asset.sha256 =
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                .toHex()
                .toStdString();
        result.asset.byteSize = upload.second.byteSize;
        result.asset.kind = daw::AssetKind::Audio;
        result.asset.originalName = upload.second.displayName.toStdString();
        result.asset.mimeType = upload.second.contentType.toStdString();
        publisher.m_impl->assetCompleted(upload.first, result);
    };
    const QByteArray firstBytes =
        assetUploads.front().second.assetId == clipAssetId ? clipBytes
                                                           : sampleBytes;
    completeAsset(0, firstBytes);
    if (assetUploads.size() != 2 || !snapshotUploads.isEmpty()) {
        return fail(QStringLiteral(
            "publisher advanced before every bounded asset completed"));
    }
    const QByteArray secondBytes =
        assetUploads.at(1).second.assetId == clipAssetId ? clipBytes
                                                         : sampleBytes;
    completeAsset(1, secondBytes);
    if (snapshotUploads.size() != 1 ||
        publisher.phase() != CloudPublicationPhase::UploadingSnapshot ||
        !activations.isEmpty() ||
        snapshotUploads.front().second.assetIds !=
            QStringList{clipAssetId, sampleAssetId}) {
        return fail(QStringLiteral(
            "publisher did not stage exactly one canonical snapshot"));
    }

    const auto snapshotUpload = snapshotUploads.front();
    CloudSnapshotUploadResult snapshot;
    snapshot.projectId = projectId;
    snapshot.uploadId = snapshotUpload.second.uploadId;
    snapshot.snapshotId =
        QStringLiteral("77000000-0000-4000-8000-000000000001");
    snapshot.blobId =
        QStringLiteral("78000000-0000-4000-8000-000000000001");
    snapshot.sequence = 0;
    snapshot.schemaVersion = daw::ProjectSerializer::kFormatVersion;
    snapshot.sha256 = snapshotUpload.second.sha256;
    snapshot.byteSize = snapshotUpload.second.byteSize;
    snapshot.contentType = snapshotUpload.second.contentType;
    snapshot.assetIds = snapshotUpload.second.assetIds;
    publisher.m_impl->snapshotCompleted(snapshotUpload.first, snapshot);
    if (publisher.phase() != CloudPublicationPhase::Activating ||
        activations != QVector<QString>{projectId}) {
        return fail(QStringLiteral(
            "publisher activated before a verified snapshot completion"));
    }

    CloudProjectView active = uploading;
    active.project.status = CloudProjectStatus::Active;
    publisher.m_impl->projectReceived(
        publisher.m_impl->publishRequestId, CloudRequestKind::PublishProject,
        active);
    if (!completed || publisher.phase() != CloudPublicationPhase::Completed ||
        progress.isEmpty() || progress.back() != QPair<int, int>{5, 5}) {
        return fail(QStringLiteral("publisher did not complete deterministically"));
    }
    const QVector<CloudPublicationPhase> expectedPhases{
        CloudPublicationPhase::Preflight,
        CloudPublicationPhase::CreatingProject,
        CloudPublicationPhase::UploadingAssets,
        CloudPublicationPhase::PreparingSnapshot,
        CloudPublicationPhase::UploadingSnapshot,
        CloudPublicationPhase::Activating,
        CloudPublicationPhase::Completed,
    };
    if (phases != expectedPhases ||
        daw::cloud::containsLocalPathOrUiState(
            completedResult.canonicalDocument.project) ||
        completedResult.canonicalDocument.confirmedSequence != 0) {
        return fail(QStringLiteral(
            "publisher phases or canonical seq-0 document are invalid"));
    }
    const daw::AssetRef& canonicalClip =
        completedResult.canonicalDocument.project.tracks.front()
            .clips.front()
            .asset;
    if (canonicalClip.sha256.empty() || canonicalClip.codec != "pcm_s16le" ||
        canonicalClip.sampleRate != 48000.0 || canonicalClip.channels != 2 ||
        canonicalClip.frames != 128 ||
        completedResult.canonicalDocument.project.tracks.front()
            .instrument.assetBindings.front()
            .asset.sha256.empty() ||
        source.tracks.front().clips.front().filePath != clipPath.toStdString() ||
        !source.tracks.front().clips.front().asset.sha256.empty() ||
        !source.tracks.front()
             .instrument.assetBindings.front()
             .asset.sha256.empty()) {
        return fail(QStringLiteral(
            "publisher lost audio metadata or mutated the caller document"));
    }

    const int uploadsBeforeCancel = assetUploads.size();
    publisher.publish(input);
    const quint64 staleCreate = publisher.m_impl->createRequestId;
    publisher.publish(input);
    const quint64 currentCreate = publisher.m_impl->createRequestId;
    publisher.m_impl->projectReceived(staleCreate,
                                      CloudRequestKind::CreateProject,
                                      uploading);
    if (publisher.m_impl->createRequestId != currentCreate ||
        assetUploads.size() != uploadsBeforeCancel) {
        return fail(QStringLiteral(
            "new publication generation accepted an older callback"));
    }
    publisher.cancel();
    publisher.m_impl->projectReceived(currentCreate,
                                      CloudRequestKind::CreateProject,
                                      uploading);
    if (publisher.phase() != CloudPublicationPhase::Cancelled ||
        assetUploads.size() != uploadsBeforeCancel ||
        cancelledRequests.isEmpty()) {
        return fail(QStringLiteral(
            "cancelled publication accepted a stale create callback"));
    }

    CloudProjectPublicationInput zeroAssets;
    zeroAssets.metadata = input.metadata;
    const int snapshotsBeforeZero = snapshotUploads.size();
    publisher.publish(zeroAssets);
    CloudProjectView zeroUploading = uploading;
    zeroUploading.project.id =
        QStringLiteral("79000000-0000-4000-8000-000000000001");
    publisher.m_impl->projectReceived(
        publisher.m_impl->createRequestId, CloudRequestKind::CreateProject,
        zeroUploading);
    if (assetUploads.size() != uploadsBeforeCancel ||
        snapshotUploads.size() != snapshotsBeforeZero + 1 ||
        publisher.phase() != CloudPublicationPhase::UploadingSnapshot) {
        return fail(QStringLiteral(
            "zero-asset publication did not advance directly to snapshot"));
    }
    CloudTransferError retryableSnapshotError;
    retryableSnapshotError.safeMessage =
        QStringLiteral("temporary snapshot failure");
    retryableSnapshotError.retryable = true;
    const quint64 zeroSnapshotTransfer = snapshotUploads.back().first;
    publisher.m_impl->transferFailed(zeroSnapshotTransfer,
                                     CloudTransferKind::SnapshotUpload,
                                     retryableSnapshotError);
    publisher.m_impl->retrySnapshot(publisher.generation(),
                                    zeroSnapshotTransfer);
    if (retriedTransfers.isEmpty() ||
        retriedTransfers.back() != zeroSnapshotTransfer) {
        return fail(QStringLiteral(
            "publisher did not resume a retryable snapshot upload"));
    }
    publisher.cancel();
    if (abortedUploads != QVector<QPair<QString, QString>>{
                              {zeroUploading.project.id,
                               snapshotUploads.back().second.uploadId}}) {
        return fail(QStringLiteral(
            "publisher cancellation did not abort the server upload"));
    }
    return true;
}

} // namespace collab
