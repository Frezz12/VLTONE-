#include "CloudRecordingAssetCoordinator.hpp"

#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace collab {
namespace {

constexpr qsizetype kMaximumRecordingAssets = 8;
constexpr quint64 kMaximumBlobBytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr qsizetype kMaximumSafeMessage = 512;

QString canonicalUuid(const QString& value) {
    const QUuid parsed(value);
    if (parsed.isNull()) return {};
    const QString canonical =
        parsed.toString(QUuid::WithoutBraces).toLower();
    return value == canonical ? canonical : QString();
}

bool lowercaseSha256(const QString& value) {
    if (value.size() != 64) return false;
    return std::all_of(value.cbegin(), value.cend(), [](QChar character) {
        const ushort code = character.unicode();
        return (code >= '0' && code <= '9') ||
               (code >= 'a' && code <= 'f');
    });
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

bool safeText(const QString& value, qsizetype maximum) {
    return !value.isEmpty() && value.size() <= maximum &&
           !value.contains(QChar::Null) &&
           !value.contains(QLatin1Char('\r')) &&
           !value.contains(QLatin1Char('\n'));
}

QString boundedSafeMessage(QString message, const QString& fallback) {
    message.replace(QChar::Null, QLatin1Char(' '));
    message.replace(QLatin1Char('\r'), QLatin1Char(' '));
    message.replace(QLatin1Char('\n'), QLatin1Char(' '));
    message = message.simplified();
    if (message.isEmpty()) message = fallback;
    return message.left(kMaximumSafeMessage);
}

bool sameAudioMetadata(const daw::AssetRef& asset,
                       const ClosedRecordingAsset& recording) {
    return asset.kind == daw::AssetKind::Audio &&
           asset.originalName == recording.displayName.toStdString() &&
           asset.mimeType == recording.contentType.toStdString() &&
           asset.codec == recording.codec.toStdString() &&
           asset.sampleRate == recording.sampleRate &&
           asset.channels == recording.channels &&
           asset.frames == recording.frames;
}

class DefaultCloudRecordingAssetPort final
    : public CloudRecordingAssetPort {
public:
    DefaultCloudRecordingAssetPort(CloudAssetTransferManager* transfers,
                                   AssetCache* cache)
        : m_transfers(transfers), m_cache(cache) {
        m_importPool.setMaxThreadCount(2);
        m_importPool.setExpiryTimeout(30000);
        if (m_transfers) {
            connect(m_transfers,
                    &CloudAssetTransferManager::assetUploadCompleted,
                    this,
                    [this](quint64 id,
                           const CloudAssetUploadResult& result) {
                        emit assetUploadCompleted(id, result);
                    });
            connect(m_transfers,
                    &CloudAssetTransferManager::transferFailed,
                    this,
                    [this](quint64 id, CloudTransferKind kind,
                           const CloudTransferError& error) {
                        emit transferFailed(id, kind, error);
                    });
            connect(m_transfers, &QObject::destroyed, this, [this] {
                m_transfers = nullptr;
                emit unavailable();
            });
        }
        if (m_cache) {
            connect(m_cache, &QObject::destroyed, this, [this] {
                m_cache = nullptr;
                emit unavailable();
            });
        }
    }

    ~DefaultCloudRecordingAssetPort() override {
        m_shuttingDown.store(true, std::memory_order_release);
        for (const auto& cancelled : std::as_const(m_importCancellation))
            cancelled->store(true, std::memory_order_release);
        m_importPool.clear();
        m_importPool.waitForDone();
        m_importCancellation.clear();
    }

    quint64 importFile(const daw::AssetRef& expected,
                       const QString& sourcePath) override {
        AssetCache* cache = m_cache.data();
        if (!cache || m_shuttingDown.load(std::memory_order_acquire)) return 0;
        quint64 requestId = ++m_nextImportRequestId;
        if (requestId == 0) requestId = ++m_nextImportRequestId;
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        m_importCancellation.insert(requestId, cancelled);

        // AssetCache::importFile is explicitly a worker-only API. The cache and
        // transfer manager must outlive this production adapter; its destructor
        // waits for every in-flight import before releasing either dependency.
        m_importPool.start(
            [this, cache, requestId, expected, sourcePath, cancelled] {
                AssetCacheResult result;
                if (!cancelled->load(std::memory_order_acquire) &&
                    !m_shuttingDown.load(std::memory_order_acquire)) {
                    result = cache->importFile(expected, sourcePath);
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, requestId, result = std::move(result), cancelled] {
                        m_importCancellation.remove(requestId);
                        if (cancelled->load(std::memory_order_acquire) ||
                            m_shuttingDown.load(std::memory_order_acquire)) {
                            return;
                        }
                        if (result) {
                            emit importCompleted(requestId, result);
                        } else {
                            // AssetCache diagnostics can contain platform I/O
                            // detail. Keep the collaboration boundary generic.
                            emit importFailed(
                                requestId,
                                QStringLiteral(
                                    "Recording could not be prepared for upload"),
                                true);
                        }
                    },
                    Qt::QueuedConnection);
            });
        return requestId;
    }

    bool cancelImport(quint64 requestId) override {
        const auto found = m_importCancellation.constFind(requestId);
        if (found == m_importCancellation.cend()) return false;
        found.value()->store(true, std::memory_order_release);
        return true;
    }

    quint64 uploadAsset(const CloudAssetUploadInput& input) override {
        return m_transfers ? m_transfers->uploadAsset(input) : 0;
    }

    bool retryUpload(quint64 transferId) override {
        return m_transfers && m_transfers->retry(transferId);
    }

    bool cancelUpload(quint64 transferId) override {
        return m_transfers && m_transfers->cancel(transferId);
    }

private:
    QPointer<CloudAssetTransferManager> m_transfers;
    QPointer<AssetCache> m_cache;
    QThreadPool m_importPool;
    QHash<quint64, std::shared_ptr<std::atomic_bool>> m_importCancellation;
    std::atomic_bool m_shuttingDown{false};
    quint64 m_nextImportRequestId = 0;
};

} // namespace

struct CloudRecordingAssetCoordinator::Impl {
    enum class FailureStage : quint8 { None, Import, Upload };

    struct Item {
        ClosedRecordingAsset recording;
        CloudRecordingAssetItemState state =
            CloudRecordingAssetItemState::Importing;
        AssetCacheResult imported;
        CloudRecordingAssetResult result;
        FailureStage failureStage = FailureStage::None;
        quint64 importRequestId = 0;
        quint64 transferId = 0;
        QString safeMessage;
        bool retryable = false;
    };

    struct RequestContext {
        quint64 generation = 0;
        int itemIndex = -1;
    };

    struct DeferredEvent {
        enum class Type : quint8 {
            ImportCompleted,
            ImportFailed,
            UploadCompleted,
            UploadFailed,
            Unavailable,
        };
        Type type = Type::Unavailable;
        quint64 requestId = 0;
        AssetCacheResult importResult;
        CloudAssetUploadResult uploadResult;
        CloudTransferKind transferKind = CloudTransferKind::AssetUpload;
        CloudTransferError transferError;
        QString safeMessage;
        bool retryable = false;
    };

    explicit Impl(CloudRecordingAssetCoordinator& ownerIn)
        : owner(ownerIn) {}

    ~Impl() {
        // The owned port is a QObject whose destroyed() signal is connected to
        // owner with a lambda capturing this Impl. Tear that connection down
        // while every Impl member is still alive, before normal reverse member
        // destruction reaches ownedPort.
        if (port) QObject::disconnect(port, nullptr, &owner, nullptr);
        ownedPort.reset();
        port = nullptr;
    }

    void initialize(CloudRecordingAssetPort* nextPort) {
        port = nextPort;
        if (!port) return;
        QObject::connect(
            port, &CloudRecordingAssetPort::importCompleted, &owner,
            [this](quint64 id, const AssetCacheResult& result) {
                DeferredEvent event;
                event.type = DeferredEvent::Type::ImportCompleted;
                event.requestId = id;
                event.importResult = result;
                receive(std::move(event));
            });
        QObject::connect(
            port, &CloudRecordingAssetPort::importFailed, &owner,
            [this](quint64 id, const QString& message, bool retryable) {
                DeferredEvent event;
                event.type = DeferredEvent::Type::ImportFailed;
                event.requestId = id;
                event.safeMessage = message;
                event.retryable = retryable;
                receive(std::move(event));
            });
        QObject::connect(
            port, &CloudRecordingAssetPort::assetUploadCompleted, &owner,
            [this](quint64 id, const CloudAssetUploadResult& result) {
                DeferredEvent event;
                event.type = DeferredEvent::Type::UploadCompleted;
                event.requestId = id;
                event.uploadResult = result;
                receive(std::move(event));
            });
        QObject::connect(
            port, &CloudRecordingAssetPort::transferFailed, &owner,
            [this](quint64 id, CloudTransferKind kind,
                   const CloudTransferError& error) {
                DeferredEvent event;
                event.type = DeferredEvent::Type::UploadFailed;
                event.requestId = id;
                event.transferKind = kind;
                event.transferError = error;
                receive(std::move(event));
            });
        QObject::connect(port, &CloudRecordingAssetPort::unavailable, &owner,
                         [this] {
                             DeferredEvent event;
                             event.type = DeferredEvent::Type::Unavailable;
                             receive(std::move(event));
                         });
        QObject::connect(port, &QObject::destroyed, &owner, [this] {
            port = nullptr;
            DeferredEvent event;
            event.type = DeferredEvent::Type::Unavailable;
            receive(std::move(event));
        });
    }

    static bool normalizeRecording(ClosedRecordingAsset& recording,
                                   QString* error) {
        recording.projectId = canonicalUuid(recording.projectId);
        recording.uploadId = canonicalUuid(recording.uploadId);
        recording.assetId = canonicalUuid(recording.assetId);
        if (recording.projectId.isEmpty() || recording.uploadId.isEmpty() ||
            recording.assetId.isEmpty()) {
            if (error) *error = QStringLiteral("Recording identity is invalid");
            return false;
        }
        if (recording.sourcePath.isEmpty() ||
            !QFileInfo(recording.sourcePath).isAbsolute()) {
            if (error)
                *error = QStringLiteral("Recording source path is invalid");
            return false;
        }
        if (recording.displayName.isEmpty())
            recording.displayName = QFileInfo(recording.sourcePath).fileName();
        recording.displayName = safeBasename(recording.displayName);
        recording.contentType = recording.contentType.trimmed().toLower();
        recording.codec = recording.codec.trimmed();
        const bool wavContentType =
            recording.contentType == QLatin1String("audio/wav") ||
            recording.contentType == QLatin1String("audio/x-wav") ||
            recording.contentType == QLatin1String("audio/vnd.wave");
        if (recording.displayName.isEmpty() || !wavContentType ||
            !safeText(recording.contentType, 160) ||
            !safeText(recording.codec, 255) ||
            !std::isfinite(recording.sampleRate) ||
            recording.sampleRate <= 0.0 ||
            recording.sampleRate > 768000.0 || recording.channels == 0 ||
            recording.channels > 1024 || recording.frames == 0) {
            if (error)
                *error = QStringLiteral("Recording audio metadata is invalid");
            return false;
        }
        return true;
    }

    quint64 begin(const QVector<ClosedRecordingAsset>& requested) {
        if (state == CloudRecordingAssetState::Importing ||
            state == CloudRecordingAssetState::Uploading) {
            cancelActive(true);
        }
        ++currentGeneration;
        if (currentGeneration == 0) ++currentGeneration;
        items.clear();
        imports.clear();
        transfers.clear();
        deferred.clear();
        readyAnnounced = false;
        failureAnnounced = false;
        cancellationAnnounced = false;
        lastProgress = -1;

        if (requested.isEmpty() ||
            requested.size() > kMaximumRecordingAssets) {
            failBatchValidation(QStringLiteral(
                "A recording upload must contain between one and eight files"));
            return currentGeneration;
        }

        QSet<QString> uploadIds;
        QSet<QString> assetIds;
        QString projectId;
        QString validationError;
        items.reserve(requested.size());
        for (ClosedRecordingAsset recording : requested) {
            if (!normalizeRecording(recording, &validationError) ||
                (!projectId.isEmpty() &&
                 projectId != recording.projectId) ||
                uploadIds.contains(recording.uploadId) ||
                assetIds.contains(recording.assetId)) {
                if (validationError.isEmpty()) {
                    validationError = QStringLiteral(
                        "Recording upload identities must be unique and belong to one project");
                }
                items.clear();
                failBatchValidation(validationError);
                return currentGeneration;
            }
            projectId = recording.projectId;
            uploadIds.insert(recording.uploadId);
            assetIds.insert(recording.assetId);
            Item item;
            item.recording = std::move(recording);
            items.push_back(std::move(item));
        }

        emitProgress();
        if (!port) {
            for (int index = 0; index < items.size(); ++index) {
                failItem(index, FailureStage::Import,
                         QStringLiteral("Recording upload is unavailable"),
                         true);
            }
            evaluateState();
            return currentGeneration;
        }

        setState(CloudRecordingAssetState::Importing);
        for (int index = 0; index < items.size(); ++index)
            dispatchImport(index);
        evaluateState();
        return currentGeneration;
    }

    void failBatchValidation(const QString& message) {
        setState(CloudRecordingAssetState::Failed);
        failureAnnounced = true;
        emit owner.batchFailed(
            currentGeneration,
            boundedSafeMessage(message,
                               QStringLiteral("Recording upload is invalid")),
            false);
    }

    daw::AssetRef expectedAsset(const Item& item) const {
        daw::AssetRef asset;
        asset.assetId = item.recording.assetId.toStdString();
        asset.kind = daw::AssetKind::Audio;
        asset.originalName = item.recording.displayName.toStdString();
        asset.mimeType = item.recording.contentType.toStdString();
        asset.codec = item.recording.codec.toStdString();
        asset.sampleRate = item.recording.sampleRate;
        asset.channels = item.recording.channels;
        asset.frames = item.recording.frames;
        return asset;
    }

    void dispatchImport(int index) {
        if (!port || index < 0 || index >= items.size()) return;
        Item& item = items[index];
        item.state = CloudRecordingAssetItemState::Importing;
        item.failureStage = FailureStage::None;
        item.safeMessage.clear();
        item.retryable = false;
        item.importRequestId = 0;
        item.transferId = 0;
        ++dispatchDepth;
        const quint64 requestId =
            port->importFile(expectedAsset(item), item.recording.sourcePath);
        --dispatchDepth;
        if (requestId == 0 || imports.contains(requestId)) {
            failItem(index, FailureStage::Import,
                     QStringLiteral("Recording cache is unavailable"), true);
        } else {
            item.importRequestId = requestId;
            imports.insert(requestId, {currentGeneration, index});
        }
        drainDeferred();
    }

    CloudAssetUploadInput uploadInput(const Item& item) const {
        CloudAssetUploadInput input;
        input.projectId = item.recording.projectId;
        input.uploadId = item.recording.uploadId;
        input.assetId = item.recording.assetId;
        // Upload the verified immutable cache copy, not a recording-directory
        // file that another local workflow could rename or replace.
        input.sourcePath = item.imported.localPath;
        input.sha256 =
            QString::fromStdString(item.imported.asset.sha256);
        input.byteSize = item.imported.asset.byteSize;
        input.kind = CloudAssetKind::Audio;
        input.contentType = item.recording.contentType;
        input.displayName = item.recording.displayName;
        return input;
    }

    void dispatchUpload(int index) {
        if (!port || index < 0 || index >= items.size()) return;
        Item& item = items[index];
        item.state = CloudRecordingAssetItemState::Uploading;
        item.failureStage = FailureStage::None;
        item.safeMessage.clear();
        item.retryable = false;
        item.transferId = 0;
        ++dispatchDepth;
        const quint64 transferId = port->uploadAsset(uploadInput(item));
        --dispatchDepth;
        if (transferId == 0 || transfers.contains(transferId)) {
            failItem(index, FailureStage::Upload,
                     QStringLiteral("Recording upload could not start"), true);
        } else {
            item.transferId = transferId;
            transfers.insert(transferId, {currentGeneration, index});
        }
        drainDeferred();
    }

    void receive(DeferredEvent event) {
        if (shuttingDown) return;
        if (dispatchDepth > 0 || drainingDeferred) {
            deferred.push_back(std::move(event));
            return;
        }
        handle(std::move(event));
    }

    void drainDeferred() {
        if (dispatchDepth > 0 || drainingDeferred) return;
        drainingDeferred = true;
        while (!deferred.isEmpty()) {
            DeferredEvent event = std::move(deferred.front());
            deferred.pop_front();
            handle(std::move(event));
        }
        drainingDeferred = false;
    }

    void handle(DeferredEvent event) {
        switch (event.type) {
            case DeferredEvent::Type::ImportCompleted:
                handleImportCompleted(event.requestId, event.importResult);
                break;
            case DeferredEvent::Type::ImportFailed:
                handleImportFailed(event.requestId, event.safeMessage,
                                   event.retryable);
                break;
            case DeferredEvent::Type::UploadCompleted:
                handleUploadCompleted(event.requestId, event.uploadResult);
                break;
            case DeferredEvent::Type::UploadFailed:
                handleUploadFailed(event.requestId, event.transferKind,
                                   event.transferError);
                break;
            case DeferredEvent::Type::Unavailable:
                handleUnavailable();
                break;
        }
    }

    std::optional<RequestContext> takeContext(
        QHash<quint64, RequestContext>& requests, quint64 requestId) {
        const auto found = requests.find(requestId);
        if (found == requests.end()) return std::nullopt;
        const RequestContext context = found.value();
        requests.erase(found);
        if (context.generation != currentGeneration || context.itemIndex < 0 ||
            context.itemIndex >= items.size()) {
            return std::nullopt;
        }
        return context;
    }

    bool validImported(const Item& item,
                       const AssetCacheResult& imported) const {
        const daw::AssetRef& asset = imported.asset;
        return imported.ok && !imported.localPath.isEmpty() &&
               QFileInfo(imported.localPath).isAbsolute() &&
               QString::fromStdString(asset.assetId) ==
                   item.recording.assetId &&
               lowercaseSha256(QString::fromStdString(asset.sha256)) &&
               asset.byteSize > 0 && asset.byteSize <= kMaximumBlobBytes &&
               sameAudioMetadata(asset, item.recording);
    }

    void handleImportCompleted(quint64 requestId,
                               const AssetCacheResult& imported) {
        const auto context = takeContext(imports, requestId);
        if (!context) return;
        Item& item = items[context->itemIndex];
        if (item.state != CloudRecordingAssetItemState::Importing ||
            item.importRequestId != requestId) {
            return;
        }
        item.importRequestId = 0;
        if (!validImported(item, imported)) {
            failItem(context->itemIndex, FailureStage::Import,
                     QStringLiteral("Prepared recording identity is invalid"),
                     true);
            evaluateState();
            return;
        }
        item.imported = imported;
        dispatchUpload(context->itemIndex);
        evaluateState();
    }

    void handleImportFailed(quint64 requestId, const QString& message,
                            bool retryable) {
        const auto context = takeContext(imports, requestId);
        if (!context) return;
        Item& item = items[context->itemIndex];
        if (item.state != CloudRecordingAssetItemState::Importing ||
            item.importRequestId != requestId) {
            return;
        }
        item.importRequestId = 0;
        failItem(context->itemIndex, FailureStage::Import, message, retryable);
        evaluateState();
    }

    bool validUploadResult(const Item& item,
                           const CloudAssetUploadResult& uploaded) const {
        const daw::AssetRef& asset = uploaded.asset;
        return uploaded.projectId == item.recording.projectId &&
               uploaded.uploadId == item.recording.uploadId &&
               !canonicalUuid(uploaded.blobId).isEmpty() &&
               QString::fromStdString(asset.assetId) ==
                   item.recording.assetId &&
               QString::fromStdString(asset.sha256) ==
                   QString::fromStdString(item.imported.asset.sha256) &&
               asset.byteSize == item.imported.asset.byteSize &&
               asset.kind == daw::AssetKind::Audio &&
               asset.originalName == item.recording.displayName.toStdString() &&
               asset.mimeType == item.recording.contentType.toStdString() &&
               uploaded.contentType == item.recording.contentType;
    }

    void handleUploadCompleted(quint64 transferId,
                               const CloudAssetUploadResult& uploaded) {
        const auto context = takeContext(transfers, transferId);
        if (!context) return;
        Item& item = items[context->itemIndex];
        if (item.state != CloudRecordingAssetItemState::Uploading ||
            item.transferId != transferId) {
            return;
        }
        if (!validUploadResult(item, uploaded)) {
            failItem(context->itemIndex, FailureStage::Upload,
                     QStringLiteral("Completed recording metadata is invalid"),
                     true);
            evaluateState();
            return;
        }

        CloudRecordingAssetResult ready;
        ready.projectId = item.recording.projectId;
        ready.uploadId = item.recording.uploadId;
        ready.localPath = item.imported.localPath;
        ready.asset = uploaded.asset;
        // The storage service verifies immutable blob identity. Audio stream
        // metadata is known by the recorder and remains part of AssetRef.
        ready.asset.codec = item.recording.codec.toStdString();
        ready.asset.sampleRate = item.recording.sampleRate;
        ready.asset.channels = item.recording.channels;
        ready.asset.frames = item.recording.frames;

        item.result = ready;
        item.state = CloudRecordingAssetItemState::Ready;
        item.failureStage = FailureStage::None;
        item.safeMessage.clear();
        item.retryable = false;
        emit owner.assetReady(currentGeneration, ready);
        emitProgress();
        evaluateState();
    }

    void handleUploadFailed(quint64 transferId, CloudTransferKind kind,
                            const CloudTransferError& error) {
        if (kind != CloudTransferKind::AssetUpload) return;
        const auto context = takeContext(transfers, transferId);
        if (!context) return;
        Item& item = items[context->itemIndex];
        if (item.state != CloudRecordingAssetItemState::Uploading ||
            item.transferId != transferId) {
            return;
        }
        failItem(context->itemIndex, FailureStage::Upload,
                 error.safeMessage, error.retryable);
        evaluateState();
    }

    void handleUnavailable() {
        imports.clear();
        transfers.clear();
        for (int index = 0; index < items.size(); ++index) {
            if (items[index].state ==
                    CloudRecordingAssetItemState::Importing ||
                items[index].state ==
                    CloudRecordingAssetItemState::Uploading) {
                const FailureStage stage =
                    items[index].state ==
                            CloudRecordingAssetItemState::Importing
                        ? FailureStage::Import
                        : FailureStage::Upload;
                failItem(index, stage,
                         QStringLiteral("Recording upload is unavailable"),
                         true);
            }
        }
        evaluateState();
    }

    void failItem(int index, FailureStage stage, const QString& message,
                  bool canRetry) {
        if (index < 0 || index >= items.size()) return;
        Item& item = items[index];
        item.state = CloudRecordingAssetItemState::Failed;
        item.failureStage = stage;
        item.safeMessage = boundedSafeMessage(
            message, QStringLiteral("Recording upload failed"));
        item.retryable = canRetry;
        emit owner.itemFailed(currentGeneration, item.recording.uploadId,
                              item.safeMessage, item.retryable);
    }

    bool retry(const QString& requestedUploadId) {
        const QString uploadId = canonicalUuid(requestedUploadId);
        if (uploadId.isEmpty() || !port) return false;
        const auto found = std::find_if(
            items.begin(), items.end(), [&](const Item& item) {
                return item.recording.uploadId == uploadId;
            });
        if (found == items.end() ||
            found->state != CloudRecordingAssetItemState::Failed ||
            !found->retryable) {
            return false;
        }
        const int index = int(std::distance(items.begin(), found));
        failureAnnounced = false;
        readyAnnounced = false;
        if (found->failureStage == FailureStage::Import) {
            dispatchImport(index);
            evaluateState();
            return true;
        }
        if (found->failureStage != FailureStage::Upload || !found->imported)
            return false;

        const quint64 previousId = found->transferId;
        if (previousId != 0) {
            found->state = CloudRecordingAssetItemState::Uploading;
            found->safeMessage.clear();
            found->retryable = false;
            transfers.insert(previousId, {currentGeneration, index});
            ++dispatchDepth;
            const bool resumed = port->retryUpload(previousId);
            --dispatchDepth;
            drainDeferred();
            if (resumed) {
                evaluateState();
                return true;
            }
            // A synchronous fake may have completed despite returning false;
            // do not start a second upload after any terminal callback.
            if (items[index].state !=
                CloudRecordingAssetItemState::Uploading) {
                evaluateState();
                return true;
            }
            transfers.remove(previousId);
        }
        dispatchUpload(index);
        evaluateState();
        return items[index].state != CloudRecordingAssetItemState::Failed ||
               items[index].transferId != 0;
    }

    void retryFailed() {
        QStringList failed;
        for (const Item& item : std::as_const(items)) {
            if (item.state == CloudRecordingAssetItemState::Failed &&
                item.retryable) {
                failed.push_back(item.recording.uploadId);
            }
        }
        for (const QString& uploadId : failed) retry(uploadId);
    }

    void cancelActive(bool announce) {
        const quint64 cancelledGeneration = currentGeneration;

        // Detach every correlation before entering the port. A port is allowed
        // to emit a terminal callback synchronously from cancel*, and that
        // callback must see stale work rather than mutate the containers being
        // traversed or publish a second batch terminal signal.
        QSet<quint64> importIds;
        QSet<quint64> transferIds;
        for (auto found = imports.cbegin(); found != imports.cend(); ++found)
            importIds.insert(found.key());
        for (auto found = transfers.cbegin(); found != transfers.cend();
             ++found) {
            transferIds.insert(found.key());
        }
        for (const Item& item : std::as_const(items)) {
            if (item.importRequestId != 0 &&
                (item.state == CloudRecordingAssetItemState::Importing ||
                 (item.state == CloudRecordingAssetItemState::Failed &&
                  item.failureStage == FailureStage::Import))) {
                importIds.insert(item.importRequestId);
            }
            if (item.transferId != 0 &&
                (item.state == CloudRecordingAssetItemState::Uploading ||
                 (item.state == CloudRecordingAssetItemState::Failed &&
                  item.failureStage == FailureStage::Upload))) {
                transferIds.insert(item.transferId);
            }
        }
        imports.clear();
        transfers.clear();
        deferred.clear();
        for (Item& item : items) {
            if (item.state != CloudRecordingAssetItemState::Ready) {
                item.state = CloudRecordingAssetItemState::Cancelled;
                item.importRequestId = 0;
                item.transferId = 0;
                item.retryable = false;
            }
        }

        // QPointer is deliberately re-checked before every call: a hostile or
        // test port may destroy itself synchronously while cancelling the first
        // request. Remaining calls then become impossible but never dereference
        // the deleted port.
        QPointer<CloudRecordingAssetPort> cancellationPort = port;
        for (quint64 requestId : std::as_const(importIds)) {
            if (!cancellationPort) break;
            cancellationPort->cancelImport(requestId);
        }
        for (quint64 transferId : std::as_const(transferIds)) {
            if (!cancellationPort) break;
            cancellationPort->cancelUpload(transferId);
        }
        if (!items.isEmpty()) {
            setState(CloudRecordingAssetState::Cancelled);
            if (announce && !shuttingDown && !cancellationAnnounced) {
                cancellationAnnounced = true;
                emit owner.batchCancelled(cancelledGeneration);
            }
        } else {
            setState(CloudRecordingAssetState::Idle);
        }
    }

    void cancel() {
        if (state != CloudRecordingAssetState::Importing &&
            state != CloudRecordingAssetState::Uploading &&
            state != CloudRecordingAssetState::Failed) {
            return;
        }
        cancelActive(true);
    }

    void setState(CloudRecordingAssetState next) {
        if (state == next) return;
        state = next;
        if (shuttingDown) return;
        emit owner.stateChanged(currentGeneration, state);
    }

    void shutdown() {
        if (shuttingDown) return;
        shuttingDown = true;
        cancelActive(false);
    }

    void emitProgress() {
        qsizetype completed = 0;
        for (const Item& item : std::as_const(items)) {
            if (item.state == CloudRecordingAssetItemState::Ready)
                ++completed;
        }
        if (completed == lastProgress) return;
        lastProgress = completed;
        emit owner.progressChanged(currentGeneration, completed, items.size());
    }

    void evaluateState() {
        if (items.isEmpty()) return;
        const bool importing = std::any_of(
            items.cbegin(), items.cend(), [](const Item& item) {
                return item.state ==
                       CloudRecordingAssetItemState::Importing;
            });
        const bool uploading = std::any_of(
            items.cbegin(), items.cend(), [](const Item& item) {
                return item.state ==
                       CloudRecordingAssetItemState::Uploading;
            });
        if (importing) {
            setState(CloudRecordingAssetState::Importing);
            return;
        }
        if (uploading) {
            setState(CloudRecordingAssetState::Uploading);
            return;
        }
        const bool allReady = std::all_of(
            items.cbegin(), items.cend(), [](const Item& item) {
                return item.state == CloudRecordingAssetItemState::Ready;
            });
        if (allReady) {
            setState(CloudRecordingAssetState::Ready);
            if (!readyAnnounced) {
                readyAnnounced = true;
                QVector<CloudRecordingAssetResult> results;
                results.reserve(items.size());
                for (const Item& item : std::as_const(items))
                    results.push_back(item.result);
                emit owner.batchReady(currentGeneration, results);
            }
            return;
        }
        const bool anyFailed = std::any_of(
            items.cbegin(), items.cend(), [](const Item& item) {
                return item.state == CloudRecordingAssetItemState::Failed;
            });
        if (anyFailed) {
            setState(CloudRecordingAssetState::Failed);
            if (!failureAnnounced) {
                failureAnnounced = true;
                QString message = QStringLiteral("Recording upload failed");
                bool canRetry = false;
                for (const Item& item : std::as_const(items)) {
                    if (item.state != CloudRecordingAssetItemState::Failed)
                        continue;
                    if (!item.safeMessage.isEmpty()) message = item.safeMessage;
                    canRetry = canRetry || item.retryable;
                }
                emit owner.batchFailed(currentGeneration, message, canRetry);
            }
            return;
        }
        setState(CloudRecordingAssetState::Cancelled);
    }

    QVector<CloudRecordingAssetRecoveryItem> recoveryItems() const {
        QVector<CloudRecordingAssetRecoveryItem> recovery;
        recovery.reserve(items.size());
        for (const Item& item : items) {
            CloudRecordingAssetRecoveryItem entry;
            entry.recording = item.recording;
            entry.state = item.state;
            entry.cachedPath = item.imported.localPath;
            entry.sha256 = QString::fromStdString(item.imported.asset.sha256);
            entry.byteSize = item.imported.asset.byteSize;
            entry.safeMessage = item.safeMessage;
            entry.retryable = item.retryable;
            recovery.push_back(std::move(entry));
        }
        return recovery;
    }

    CloudRecordingAssetCoordinator& owner;
    std::unique_ptr<CloudRecordingAssetPort> ownedPort;
    QPointer<CloudRecordingAssetPort> port;
    QVector<Item> items;
    QHash<quint64, RequestContext> imports;
    QHash<quint64, RequestContext> transfers;
    QVector<DeferredEvent> deferred;
    CloudRecordingAssetState state = CloudRecordingAssetState::Idle;
    quint64 currentGeneration = 0;
    int dispatchDepth = 0;
    bool drainingDeferred = false;
    bool readyAnnounced = false;
    bool failureAnnounced = false;
    bool cancellationAnnounced = false;
    bool shuttingDown = false;
    qsizetype lastProgress = -1;
};

CloudRecordingAssetCoordinator::CloudRecordingAssetCoordinator(
    CloudAssetTransferManager* transfers, AssetCache* cache, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {
    m_impl->ownedPort =
        std::make_unique<DefaultCloudRecordingAssetPort>(transfers, cache);
    m_impl->initialize(m_impl->ownedPort.get());
}

CloudRecordingAssetCoordinator::CloudRecordingAssetCoordinator(
    CloudRecordingAssetPort* port, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {
    m_impl->initialize(port);
}

CloudRecordingAssetCoordinator::~CloudRecordingAssetCoordinator() {
    if (m_impl) {
        // QObject teardown must not publish a final UI state transition. The
        // port cancellation still invalidates every asynchronous callback.
        m_impl->shutdown();
    }
}

quint64 CloudRecordingAssetCoordinator::begin(
    const QVector<ClosedRecordingAsset>& recordings) {
    return m_impl->begin(recordings);
}

bool CloudRecordingAssetCoordinator::retry(const QString& uploadId) {
    return m_impl->retry(uploadId);
}

void CloudRecordingAssetCoordinator::retryFailed() {
    m_impl->retryFailed();
}

void CloudRecordingAssetCoordinator::cancel() {
    m_impl->cancel();
}

quint64 CloudRecordingAssetCoordinator::generation() const noexcept {
    return m_impl->currentGeneration;
}

CloudRecordingAssetState CloudRecordingAssetCoordinator::state() const
    noexcept {
    return m_impl->state;
}

QVector<CloudRecordingAssetRecoveryItem>
CloudRecordingAssetCoordinator::recoveryItems() const {
    return m_impl->recoveryItems();
}

namespace {

class FakeCloudRecordingAssetPort final : public CloudRecordingAssetPort {
public:
    struct ImportCall {
        quint64 id = 0;
        daw::AssetRef expected;
        QString sourcePath;
    };

    struct UploadCall {
        quint64 id = 0;
        CloudAssetUploadInput input;
    };

    ~FakeCloudRecordingAssetPort() override {
        // A production adapter can report a dependency loss from its own
        // teardown before QObject emits destroyed(). Exercise both signals for
        // one loss so the coordinator must keep terminal notifications exact.
        if (announceUnavailableOnDestruction) emit unavailable();
    }

    quint64 importFile(const daw::AssetRef& expected,
                       const QString& sourcePath) override {
        const quint64 id = ++nextImportId;
        imports.push_back({id, expected, sourcePath});
        if (synchronousImport) completeImport(id);
        return id;
    }

    bool cancelImport(quint64 requestId) override {
        cancelledImports.push_back(requestId);
        if (failSynchronouslyOnImportCancel) {
            failImport(requestId, QStringLiteral("cancel import callback"),
                       true);
        }
        return true;
    }

    quint64 uploadAsset(const CloudAssetUploadInput& input) override {
        const quint64 id = ++nextUploadId;
        uploads.push_back({id, input});
        if (synchronousUpload) completeUpload(id);
        return id;
    }

    bool retryUpload(quint64 transferId) override {
        retriedUploads.push_back(transferId);
        return resumeExistingUpload;
    }

    bool cancelUpload(quint64 transferId) override {
        cancelledUploads.push_back(transferId);
        if (failSynchronouslyOnUploadCancel) {
            failUpload(transferId, QStringLiteral("cancel upload callback"),
                       true);
        }
        return true;
    }

    const ImportCall* importCall(quint64 id) const {
        const auto found = std::find_if(
            imports.cbegin(), imports.cend(),
            [id](const ImportCall& call) { return call.id == id; });
        return found == imports.cend() ? nullptr : &*found;
    }

    const UploadCall* uploadCall(quint64 id) const {
        const auto found = std::find_if(
            uploads.cbegin(), uploads.cend(),
            [id](const UploadCall& call) { return call.id == id; });
        return found == uploads.cend() ? nullptr : &*found;
    }

    AssetCacheResult importedResult(quint64 id) const {
        AssetCacheResult result;
        const ImportCall* call = importCall(id);
        if (!call) return result;
        result.ok = true;
        result.asset = call->expected;
        const QChar digit = QChar::fromLatin1(
            char('a' + ((id - 1) % 6)));
        result.asset.sha256 = QString(64, digit).toStdString();
        result.asset.byteSize = 4096 + id;
        result.localPath = QStringLiteral("/cache/%1.wav").arg(id);
        return result;
    }

    CloudAssetUploadResult uploadedResult(quint64 id) const {
        CloudAssetUploadResult result;
        const UploadCall* call = uploadCall(id);
        if (!call) return result;
        result.projectId = call->input.projectId;
        result.uploadId = call->input.uploadId;
        result.blobId = QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-%1")
                            .arg(id, 12, 10, QLatin1Char('0'));
        result.contentType = call->input.contentType;
        result.asset.assetId = call->input.assetId.toStdString();
        result.asset.sha256 = call->input.sha256.toStdString();
        result.asset.byteSize = call->input.byteSize;
        result.asset.kind = daw::AssetKind::Audio;
        result.asset.originalName = call->input.displayName.toStdString();
        result.asset.mimeType = call->input.contentType.toStdString();
        return result;
    }

    void completeImport(quint64 id) {
        emit importCompleted(id, importedResult(id));
    }

    void failImport(quint64 id, const QString& message, bool retryable) {
        emit importFailed(id, message, retryable);
    }

    void completeUpload(quint64 id) {
        emit assetUploadCompleted(id, uploadedResult(id));
    }

    void completeUpload(quint64 id,
                        const CloudAssetUploadResult& result) {
        emit assetUploadCompleted(id, result);
    }

    void failUpload(quint64 id, const QString& message, bool retryable) {
        CloudTransferError failure;
        failure.code = CloudTransferErrorCode::NetworkFailure;
        failure.safeMessage = message;
        failure.retryable = retryable;
        emit transferFailed(id, CloudTransferKind::AssetUpload, failure);
    }

    void announceUnavailable() { emit unavailable(); }

    QVector<ImportCall> imports;
    QVector<UploadCall> uploads;
    QVector<quint64> cancelledImports;
    QVector<quint64> cancelledUploads;
    QVector<quint64> retriedUploads;
    quint64 nextImportId = 0;
    quint64 nextUploadId = 0;
    bool synchronousImport = false;
    bool synchronousUpload = false;
    bool resumeExistingUpload = false;
    bool failSynchronouslyOnImportCancel = false;
    bool failSynchronouslyOnUploadCancel = false;
    bool announceUnavailableOnDestruction = false;
};

ClosedRecordingAsset testRecording(int ordinal) {
    ClosedRecordingAsset recording;
    recording.projectId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    recording.uploadId =
        QStringLiteral("22222222-2222-4222-8222-%1")
            .arg(ordinal, 12, 10, QLatin1Char('0'));
    recording.assetId =
        QStringLiteral("33333333-3333-4333-8333-%1")
            .arg(ordinal, 12, 10, QLatin1Char('0'));
    recording.sourcePath =
        QStringLiteral("/recordings/take-%1.wav").arg(ordinal);
    recording.displayName = QStringLiteral("take-%1.wav").arg(ordinal);
    recording.codec = QStringLiteral("pcm_s24le");
    recording.sampleRate = 48000.0;
    recording.channels = ordinal % 2 == 0 ? 2 : 1;
    recording.frames = 48000ULL * quint64(ordinal + 1);
    return recording;
}

} // namespace

bool checkCloudRecordingAssetCoordinatorForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };

    // Both cache and network callbacks may legally arrive synchronously from a
    // fake port. Correlation must be installed before either callback is used.
    FakeCloudRecordingAssetPort synchronous;
    synchronous.synchronousImport = true;
    synchronous.synchronousUpload = true;
    CloudRecordingAssetCoordinator coordinator(&synchronous);
    QVector<CloudRecordingAssetResult> ready;
    int batchReadyCount = 0;
    QObject::connect(
        &coordinator, &CloudRecordingAssetCoordinator::assetReady,
        [&ready](quint64, const CloudRecordingAssetResult& result) {
            ready.push_back(result);
        });
    QObject::connect(
        &coordinator, &CloudRecordingAssetCoordinator::batchReady,
        [&batchReadyCount](quint64,
                           const QVector<CloudRecordingAssetResult>&) {
            ++batchReadyCount;
        });
    const ClosedRecordingAsset first = testRecording(1);
    const ClosedRecordingAsset second = testRecording(2);
    const quint64 generation = coordinator.begin({first, second});
    if (generation == 0 || coordinator.state() !=
                               CloudRecordingAssetState::Ready ||
        ready.size() != 2 || batchReadyCount != 1 ||
        synchronous.imports.size() != 2 || synchronous.uploads.size() != 2) {
        return fail(QStringLiteral(
            "synchronous recording upload callbacks were not correlated"));
    }
    for (int index = 0; index < synchronous.uploads.size(); ++index) {
        const auto& upload = synchronous.uploads[index].input;
        const auto& import = synchronous.imports[index];
        const AssetCacheResult imported =
            synchronous.importedResult(import.id);
        const ClosedRecordingAsset& expected = index == 0 ? first : second;
        if (upload.projectId != expected.projectId ||
            upload.uploadId != expected.uploadId ||
            upload.assetId != expected.assetId ||
            upload.sourcePath != imported.localPath ||
            upload.sha256 !=
                QString::fromStdString(imported.asset.sha256) ||
            upload.byteSize != imported.asset.byteSize ||
            upload.kind != CloudAssetKind::Audio ||
            upload.contentType != expected.contentType ||
            upload.displayName != expected.displayName ||
            ready[index].asset.codec != expected.codec.toStdString() ||
            ready[index].asset.sampleRate != expected.sampleRate ||
            ready[index].asset.channels != expected.channels ||
            ready[index].asset.frames != expected.frames) {
            return fail(QStringLiteral(
                "recording upload payload or audio metadata drifted"));
        }
    }

    // Replacing a generation cancels old work. A late callback from it must not
    // start an upload in the new generation.
    FakeCloudRecordingAssetPort controlled;
    CloudRecordingAssetCoordinator races(&controlled);
    int raceReadyCount = 0;
    QObject::connect(
        &races, &CloudRecordingAssetCoordinator::assetReady,
        [&raceReadyCount](quint64, const CloudRecordingAssetResult&) {
            ++raceReadyCount;
        });
    const ClosedRecordingAsset oldRecording = testRecording(3);
    const ClosedRecordingAsset currentRecording = testRecording(4);
    races.begin({oldRecording});
    const quint64 staleImportId = controlled.imports.back().id;
    races.begin({currentRecording});
    const quint64 currentImportId = controlled.imports.back().id;
    controlled.completeImport(staleImportId);
    if (!controlled.uploads.isEmpty())
        return fail(QStringLiteral("stale cache completion started an upload"));
    controlled.completeImport(currentImportId);
    if (controlled.uploads.size() != 1 || raceReadyCount != 0)
        return fail(QStringLiteral(
            "cache import exposed an AssetRef before upload completion"));
    const quint64 mismatchedTransfer = controlled.uploads.back().id;
    CloudAssetUploadResult mismatch =
        controlled.uploadedResult(mismatchedTransfer);
    mismatch.projectId =
        QStringLiteral("99999999-9999-4999-8999-999999999999");
    controlled.completeUpload(mismatchedTransfer, mismatch);
    if (races.state() != CloudRecordingAssetState::Failed ||
        raceReadyCount != 0) {
        return fail(QStringLiteral("mismatched upload completion was accepted"));
    }

    // A resumable retry reuses the same transfer and stable upload identity.
    const ClosedRecordingAsset retryRecording = testRecording(5);
    races.begin({retryRecording});
    const quint64 retryImport = controlled.imports.back().id;
    controlled.completeImport(retryImport);
    const quint64 retryTransfer = controlled.uploads.back().id;
    controlled.failUpload(retryTransfer,
                          QStringLiteral("Temporary network failure"), true);
    controlled.resumeExistingUpload = true;
    if (!races.retry(retryRecording.uploadId) ||
        controlled.retriedUploads.isEmpty() ||
        controlled.retriedUploads.back() != retryTransfer) {
        return fail(QStringLiteral("failed recording upload did not resume"));
    }
    controlled.completeUpload(retryTransfer);
    if (races.state() != CloudRecordingAssetState::Ready ||
        raceReadyCount != 1 ||
        controlled.uploads.back().input.uploadId != retryRecording.uploadId) {
        return fail(QStringLiteral("resumed recording upload did not complete"));
    }

    // Cancellation retains only in-memory recovery metadata and makes every
    // later port response stale.
    const ClosedRecordingAsset cancelledRecording = testRecording(6);
    races.begin({cancelledRecording});
    const quint64 cancelledImport = controlled.imports.back().id;
    const qsizetype uploadsBeforeCancel = controlled.uploads.size();
    races.cancel();
    controlled.completeImport(cancelledImport);
    const QVector<CloudRecordingAssetRecoveryItem> recovery =
        races.recoveryItems();
    if (races.state() != CloudRecordingAssetState::Cancelled ||
        controlled.cancelledImports.isEmpty() ||
        controlled.cancelledImports.back() != cancelledImport ||
        controlled.uploads.size() != uploadsBeforeCancel ||
        recovery.size() != 1 ||
        recovery.front().recording.uploadId !=
            cancelledRecording.uploadId ||
        recovery.front().state !=
            CloudRecordingAssetItemState::Cancelled) {
        return fail(QStringLiteral("recording cancellation lost recovery state"));
    }

    // A port can disappear independently of the coordinator. Both import and
    // upload phases must become retryable failures instead of remaining stuck,
    // and a multi-item loss still has exactly one batch terminal notification.
    {
        auto disappearing = std::make_unique<FakeCloudRecordingAssetPort>();
        disappearing->announceUnavailableOnDestruction = true;
        CloudRecordingAssetCoordinator lostPort(disappearing.get());
        int failedItems = 0;
        int failedBatches = 0;
        int failedStates = 0;
        QObject::connect(
            &lostPort, &CloudRecordingAssetCoordinator::itemFailed,
            [&failedItems](quint64, const QString&, const QString&, bool) {
                ++failedItems;
            });
        QObject::connect(
            &lostPort, &CloudRecordingAssetCoordinator::batchFailed,
            [&failedBatches](quint64, const QString&, bool) {
                ++failedBatches;
            });
        QObject::connect(
            &lostPort, &CloudRecordingAssetCoordinator::stateChanged,
            [&failedStates](quint64, CloudRecordingAssetState state) {
                if (state == CloudRecordingAssetState::Failed) ++failedStates;
            });
        lostPort.begin({testRecording(8), testRecording(9)});
        disappearing.reset();
        const auto lostRecovery = lostPort.recoveryItems();
        if (lostPort.state() != CloudRecordingAssetState::Failed ||
            failedItems != 2 || failedBatches != 1 || failedStates != 1 ||
            lostRecovery.size() != 2 ||
            std::any_of(lostRecovery.cbegin(), lostRecovery.cend(),
                        [](const CloudRecordingAssetRecoveryItem& item) {
                            return item.state !=
                                       CloudRecordingAssetItemState::Failed ||
                                   !item.retryable ||
                                   item.safeMessage.isEmpty();
                        }) ||
            lostPort.retry(testRecording(8).uploadId)) {
            return fail(QStringLiteral(
                "destroyed import port did not fail the batch exactly once"));
        }
    }
    {
        auto disappearing = std::make_unique<FakeCloudRecordingAssetPort>();
        disappearing->announceUnavailableOnDestruction = true;
        CloudRecordingAssetCoordinator lostPort(disappearing.get());
        int failedItems = 0;
        int failedBatches = 0;
        int failedStates = 0;
        QObject::connect(
            &lostPort, &CloudRecordingAssetCoordinator::itemFailed,
            [&failedItems](quint64, const QString&, const QString&, bool) {
                ++failedItems;
            });
        QObject::connect(
            &lostPort, &CloudRecordingAssetCoordinator::batchFailed,
            [&failedBatches](quint64, const QString&, bool) {
                ++failedBatches;
            });
        QObject::connect(
            &lostPort, &CloudRecordingAssetCoordinator::stateChanged,
            [&failedStates](quint64, CloudRecordingAssetState state) {
                if (state == CloudRecordingAssetState::Failed) ++failedStates;
            });
        lostPort.begin({testRecording(10)});
        const quint64 imported = disappearing->imports.back().id;
        disappearing->completeImport(imported);
        if (disappearing->uploads.size() != 1)
            return fail(QStringLiteral("upload destruction fixture did not upload"));
        disappearing.reset();
        const auto lostRecovery = lostPort.recoveryItems();
        if (lostPort.state() != CloudRecordingAssetState::Failed ||
            failedItems != 1 || failedBatches != 1 || failedStates != 1 ||
            lostRecovery.size() != 1 ||
            lostRecovery.front().state !=
                CloudRecordingAssetItemState::Failed ||
            !lostRecovery.front().retryable ||
            lostRecovery.front().cachedPath.isEmpty()) {
            return fail(QStringLiteral(
                "destroyed upload port did not preserve retry recovery"));
        }
    }

    // An explicit dependency-loss notification also detaches correlations.
    // Even if a poorly behaved port emits the old completion afterwards, it
    // cannot expose an asset or begin an upload, and repeated loss reports are
    // idempotent.
    {
        FakeCloudRecordingAssetPort unavailable;
        CloudRecordingAssetCoordinator lostPort(&unavailable);
        int failedItems = 0;
        int failedBatches = 0;
        int readyItems = 0;
        QObject::connect(
            &lostPort, &CloudRecordingAssetCoordinator::itemFailed,
            [&failedItems](quint64, const QString&, const QString&, bool) {
                ++failedItems;
            });
        QObject::connect(
            &lostPort, &CloudRecordingAssetCoordinator::batchFailed,
            [&failedBatches](quint64, const QString&, bool) {
                ++failedBatches;
            });
        QObject::connect(
            &lostPort, &CloudRecordingAssetCoordinator::assetReady,
            [&readyItems](quint64, const CloudRecordingAssetResult&) {
                ++readyItems;
            });
        lostPort.begin({testRecording(16)});
        const quint64 staleImport = unavailable.imports.back().id;
        unavailable.announceUnavailable();
        unavailable.completeImport(staleImport);
        unavailable.announceUnavailable();
        if (lostPort.state() != CloudRecordingAssetState::Failed ||
            failedItems != 1 || failedBatches != 1 || readyItems != 0 ||
            !unavailable.uploads.isEmpty()) {
            return fail(QStringLiteral(
                "unavailable port accepted a stale import completion"));
        }
    }

    // cancel* is an arbitrary port boundary and may synchronously report the
    // very request being cancelled. Correlations are detached first, so those
    // callbacks remain stale and cancellation is the only batch terminal event.
    {
        FakeCloudRecordingAssetPort reentrant;
        reentrant.failSynchronouslyOnImportCancel = true;
        reentrant.failSynchronouslyOnUploadCancel = true;
        CloudRecordingAssetCoordinator cancelling(&reentrant);
        int failedItems = 0;
        int failedBatches = 0;
        int cancelledBatches = 0;
        QObject::connect(
            &cancelling, &CloudRecordingAssetCoordinator::itemFailed,
            [&failedItems](quint64, const QString&, const QString&, bool) {
                ++failedItems;
            });
        QObject::connect(
            &cancelling, &CloudRecordingAssetCoordinator::batchFailed,
            [&failedBatches](quint64, const QString&, bool) {
                ++failedBatches;
            });
        QObject::connect(
            &cancelling, &CloudRecordingAssetCoordinator::batchCancelled,
            [&cancelledBatches](quint64) { ++cancelledBatches; });

        cancelling.begin({testRecording(11), testRecording(12)});
        const quint64 firstImport = reentrant.imports.front().id;
        reentrant.completeImport(firstImport);
        if (reentrant.uploads.size() != 1)
            return fail(QStringLiteral("reentrant cancellation fixture did not upload"));
        const quint64 activeUpload = reentrant.uploads.front().id;
        cancelling.cancel();
        cancelling.cancel();
        reentrant.completeImport(reentrant.imports.back().id);
        reentrant.completeUpload(activeUpload);
        if (cancelling.state() != CloudRecordingAssetState::Cancelled ||
            failedItems != 0 || failedBatches != 0 ||
            cancelledBatches != 1 || reentrant.cancelledImports.size() != 1 ||
            reentrant.cancelledUploads.size() != 1) {
            return fail(QStringLiteral(
                "synchronous cancel callbacks published duplicate terminals"));
        }
    }

    // Coordinator teardown cancels both active phases and a retained failed
    // resumable transfer, but never publishes a UI terminal signal.
    {
        FakeCloudRecordingAssetPort teardown;
        teardown.failSynchronouslyOnImportCancel = true;
        teardown.failSynchronouslyOnUploadCancel = true;
        int terminalSignals = 0;
        {
            auto destroying =
                std::make_unique<CloudRecordingAssetCoordinator>(&teardown);
            QObject::connect(
                destroying.get(),
                &CloudRecordingAssetCoordinator::batchReady,
                [&terminalSignals](quint64,
                                   const QVector<CloudRecordingAssetResult>&) {
                    ++terminalSignals;
                });
            QObject::connect(
                destroying.get(),
                &CloudRecordingAssetCoordinator::batchFailed,
                [&terminalSignals](quint64, const QString&, bool) {
                    ++terminalSignals;
                });
            QObject::connect(
                destroying.get(),
                &CloudRecordingAssetCoordinator::batchCancelled,
                [&terminalSignals](quint64) { ++terminalSignals; });
            QObject::connect(
                destroying.get(),
                &CloudRecordingAssetCoordinator::itemFailed,
                [&terminalSignals](quint64, const QString&, const QString&,
                                   bool) { ++terminalSignals; });
            QObject::connect(
                destroying.get(),
                &CloudRecordingAssetCoordinator::stateChanged,
                [&terminalSignals](quint64, CloudRecordingAssetState) {
                    ++terminalSignals;
                });
            destroying->begin({testRecording(13), testRecording(14)});
            teardown.completeImport(teardown.imports.front().id);
            terminalSignals = 0;
        }
        if (terminalSignals != 0 || teardown.cancelledImports.size() != 1 ||
            teardown.cancelledUploads.size() != 1) {
            return fail(QStringLiteral(
                "coordinator teardown did not silently cancel active jobs"));
        }

        const qsizetype cancelledBefore = teardown.cancelledUploads.size();
        {
            auto failed =
                std::make_unique<CloudRecordingAssetCoordinator>(&teardown);
            failed->begin({testRecording(15)});
            teardown.completeImport(teardown.imports.back().id);
            const quint64 failedTransfer = teardown.uploads.back().id;
            teardown.failUpload(failedTransfer,
                                QStringLiteral("retry later"), true);
            if (failed->state() != CloudRecordingAssetState::Failed)
                return fail(QStringLiteral("failed transfer fixture did not fail"));
        }
        if (teardown.cancelledUploads.size() != cancelledBefore + 1) {
            return fail(QStringLiteral(
                "coordinator teardown orphaned a failed resumable transfer"));
        }
    }

    // Invalid or unstable identities are rejected before any port call.
    FakeCloudRecordingAssetPort invalidPort;
    CloudRecordingAssetCoordinator invalid(&invalidPort);
    ClosedRecordingAsset malformed = testRecording(7);
    malformed.uploadId =
        QStringLiteral("AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA");
    invalid.begin({malformed});
    if (invalid.state() != CloudRecordingAssetState::Failed ||
        !invalidPort.imports.isEmpty()) {
        return fail(QStringLiteral("non-canonical recording identity was accepted"));
    }

    if (error) error->clear();
    return true;
}

} // namespace collab
