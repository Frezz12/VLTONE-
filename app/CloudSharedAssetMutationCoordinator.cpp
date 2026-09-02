#include "CloudSharedAssetMutationCoordinator.hpp"

#include <QDir>

#include <algorithm>
#include <utility>

namespace collab {
namespace {

CloudSharedAssetMutationState sharedState(CloudRecordingAssetState state) {
    switch (state) {
        case CloudRecordingAssetState::Idle:
            return CloudSharedAssetMutationState::Idle;
        case CloudRecordingAssetState::Importing:
            return CloudSharedAssetMutationState::Importing;
        case CloudRecordingAssetState::Uploading:
            return CloudSharedAssetMutationState::Uploading;
        case CloudRecordingAssetState::Ready:
            return CloudSharedAssetMutationState::Ready;
        case CloudRecordingAssetState::Failed:
            return CloudSharedAssetMutationState::Failed;
        case CloudRecordingAssetState::Cancelled:
            return CloudSharedAssetMutationState::Cancelled;
    }
    return CloudSharedAssetMutationState::Failed;
}

ClosedRecordingAsset recordingInput(
    const CloudSharedAssetMutationInput& input) {
    ClosedRecordingAsset asset;
    asset.projectId = input.projectId;
    asset.uploadId = input.uploadId;
    asset.assetId = input.assetId;
    asset.sourcePath = input.sourcePath;
    asset.displayName = input.displayName;
    asset.contentType = input.contentType;
    asset.codec = input.codec;
    asset.sampleRate = input.sampleRate;
    asset.channels = input.channels;
    asset.frames = input.frames;
    return asset;
}

CloudSharedAssetMutationResult sharedResult(
    const CloudRecordingAssetResult& result) {
    CloudSharedAssetMutationResult ready;
    ready.projectId = result.projectId;
    ready.uploadId = result.uploadId;
    ready.cachedLocalPath = result.localPath;
    ready.asset = result.asset;
    return ready;
}

} // namespace

struct CloudSharedAssetMutationCoordinator::Impl {
    explicit Impl(CloudSharedAssetMutationCoordinator& ownerIn,
                  CloudAssetTransferManager* transfers, AssetCache* cache)
        : owner(ownerIn), assets(
              std::make_unique<CloudRecordingAssetCoordinator>(transfers,
                                                                cache)) {
        connectSignals();
    }

    explicit Impl(CloudSharedAssetMutationCoordinator& ownerIn,
                  CloudRecordingAssetPort* port)
        : owner(ownerIn),
          assets(std::make_unique<CloudRecordingAssetCoordinator>(port)) {
        connectSignals();
    }

    void connectSignals() {
        QObject::connect(
            assets.get(), &CloudRecordingAssetCoordinator::stateChanged,
            &owner, [this](quint64 generation,
                           CloudRecordingAssetState state) {
                emit owner.stateChanged(generation, sharedState(state));
            });
        QObject::connect(
            assets.get(), &CloudRecordingAssetCoordinator::progressChanged,
            &owner, [this](quint64 generation, qsizetype completed,
                           qsizetype total) {
                emit owner.progressChanged(generation, completed, total);
            });
        QObject::connect(
            assets.get(), &CloudRecordingAssetCoordinator::assetReady,
            &owner, [this](quint64 generation,
                           const CloudRecordingAssetResult& result) {
                emit owner.assetReady(generation, sharedResult(result));
            });
        QObject::connect(
            assets.get(), &CloudRecordingAssetCoordinator::batchReady,
            &owner,
            [this](quint64 generation,
                   const QVector<CloudRecordingAssetResult>& results) {
                QVector<CloudSharedAssetMutationResult> ready;
                ready.reserve(results.size());
                for (const CloudRecordingAssetResult& result : results)
                    ready.push_back(sharedResult(result));
                emit owner.batchReady(generation, ready);
            });
        QObject::connect(
            assets.get(), &CloudRecordingAssetCoordinator::itemFailed,
            &owner, [this](quint64 generation, const QString& uploadId,
                           const QString& message, bool retryable) {
                emit owner.itemFailed(generation, uploadId, message,
                                      retryable);
            });
        QObject::connect(
            assets.get(), &CloudRecordingAssetCoordinator::batchFailed,
            &owner, [this](quint64 generation, const QString& message,
                           bool retryable) {
                emit owner.batchFailed(generation, message, retryable);
            });
        QObject::connect(
            assets.get(), &CloudRecordingAssetCoordinator::batchCancelled,
            &owner, [this](quint64 generation) {
                emit owner.batchCancelled(generation);
            });
    }

    CloudSharedAssetMutationCoordinator& owner;
    std::unique_ptr<CloudRecordingAssetCoordinator> assets;
};

CloudSharedAssetMutationCoordinator::CloudSharedAssetMutationCoordinator(
    CloudAssetTransferManager* transfers, AssetCache* cache, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, transfers, cache)) {}

CloudSharedAssetMutationCoordinator::CloudSharedAssetMutationCoordinator(
    CloudRecordingAssetPort* port, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this, port)) {}

CloudSharedAssetMutationCoordinator::~CloudSharedAssetMutationCoordinator() =
    default;

quint64 CloudSharedAssetMutationCoordinator::begin(
    const QVector<CloudSharedAssetMutationInput>& inputs) {
    QVector<ClosedRecordingAsset> assets;
    assets.reserve(inputs.size());
    for (const CloudSharedAssetMutationInput& input : inputs)
        assets.push_back(recordingInput(input));
    return m_impl->assets->begin(assets);
}

bool CloudSharedAssetMutationCoordinator::retry(const QString& uploadId) {
    return m_impl->assets->retry(uploadId);
}

void CloudSharedAssetMutationCoordinator::retryFailed() {
    m_impl->assets->retryFailed();
}

void CloudSharedAssetMutationCoordinator::cancel() {
    m_impl->assets->cancel();
}

quint64 CloudSharedAssetMutationCoordinator::generation() const noexcept {
    return m_impl->assets->generation();
}

CloudSharedAssetMutationState
CloudSharedAssetMutationCoordinator::state() const noexcept {
    return sharedState(m_impl->assets->state());
}

namespace {

class FakeSharedAssetPort final : public CloudRecordingAssetPort {
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

    quint64 importFile(const daw::AssetRef& expected,
                       const QString& sourcePath) override {
        const quint64 id = ++nextImportId;
        imports.push_back({id, expected, sourcePath});
        return id;
    }
    bool cancelImport(quint64 requestId) override {
        cancelledImports.push_back(requestId);
        return true;
    }
    quint64 uploadAsset(const CloudAssetUploadInput& input) override {
        const quint64 id = ++nextTransferId;
        uploads.push_back({id, input});
        return id;
    }
    bool retryUpload(quint64 transferId) override {
        retriedTransfers.push_back(transferId);
        return true;
    }
    bool cancelUpload(quint64 transferId) override {
        cancelledTransfers.push_back(transferId);
        return true;
    }

    const ImportCall* import(quint64 id) const {
        const auto found = std::find_if(
            imports.cbegin(), imports.cend(),
            [id](const ImportCall& call) { return call.id == id; });
        return found == imports.cend() ? nullptr : &*found;
    }
    const UploadCall* upload(quint64 id) const {
        const auto found = std::find_if(
            uploads.cbegin(), uploads.cend(),
            [id](const UploadCall& call) { return call.id == id; });
        return found == uploads.cend() ? nullptr : &*found;
    }

    void completeImport(quint64 id) {
        const ImportCall* call = import(id);
        if (!call) return;
        AssetCacheResult result;
        result.ok = true;
        result.asset = call->expected;
        result.asset.sha256 = std::string(64, char('a' + (id % 6)));
        result.asset.byteSize = 1024 + id;
        result.localPath =
            QDir(QDir::tempPath()).absoluteFilePath(
                QStringLiteral("shared-cache-%1.bin").arg(id));
        emit importCompleted(id, result);
    }

    void completeUpload(quint64 id) {
        const UploadCall* call = upload(id);
        if (!call) return;
        CloudAssetUploadResult result;
        result.projectId = call->input.projectId;
        result.uploadId = call->input.uploadId;
        result.blobId = QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-%1")
                            .arg(id, 12, 10, QLatin1Char('0'));
        result.contentType = call->input.contentType;
        result.asset.assetId = call->input.assetId.toStdString();
        result.asset.sha256 = call->input.sha256.toStdString();
        result.asset.kind = daw::AssetKind::Audio;
        result.asset.byteSize = call->input.byteSize;
        result.asset.originalName = call->input.displayName.toStdString();
        result.asset.mimeType = call->input.contentType.toStdString();
        emit assetUploadCompleted(id, result);
    }

    void failUpload(quint64 id) {
        CloudTransferError failure;
        failure.code = CloudTransferErrorCode::NetworkFailure;
        failure.safeMessage = QStringLiteral("Temporary upload failure");
        failure.retryable = true;
        emit transferFailed(id, CloudTransferKind::AssetUpload, failure);
    }

    QVector<ImportCall> imports;
    QVector<UploadCall> uploads;
    QVector<quint64> cancelledImports;
    QVector<quint64> cancelledTransfers;
    QVector<quint64> retriedTransfers;
    quint64 nextImportId = 0;
    quint64 nextTransferId = 0;
};

CloudSharedAssetMutationInput sharedTestInput(int ordinal,
                                              QString contentType) {
    CloudSharedAssetMutationInput input;
    input.projectId =
        QStringLiteral("11111111-1111-4111-8111-111111111111");
    input.uploadId = QStringLiteral("22222222-2222-4222-8222-%1")
                         .arg(ordinal, 12, 10, QLatin1Char('0'));
    input.assetId = QStringLiteral("33333333-3333-4333-8333-%1")
                        .arg(ordinal, 12, 10, QLatin1Char('0'));
    const QString suffix = contentType == QLatin1String("audio/flac")
                               ? QStringLiteral("flac")
                               : QStringLiteral("wav");
    input.sourcePath = QDir(QDir::tempPath()).absoluteFilePath(
        QStringLiteral("private/source-%1.%2").arg(ordinal).arg(suffix));
    input.displayName =
        QStringLiteral("sample-%1.%2").arg(ordinal).arg(suffix);
    input.contentType = std::move(contentType);
    input.codec = suffix == QLatin1String("flac")
                      ? QStringLiteral("flac")
                      : QStringLiteral("pcm_s24le");
    input.sampleRate = 48000.0;
    input.channels = ordinal % 2 == 0 ? 2 : 1;
    input.frames = 48000ULL * quint64(ordinal + 1);
    return input;
}

} // namespace

bool checkCloudSharedAssetMutationCoordinatorForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };

    FakeSharedAssetPort port;
    CloudSharedAssetMutationCoordinator coordinator(&port);
    QVector<CloudSharedAssetMutationResult> itemResults;
    QVector<CloudSharedAssetMutationResult> batchResults;
    int cancelled = 0;
    QObject::connect(
        &coordinator, &CloudSharedAssetMutationCoordinator::assetReady,
        [&itemResults](quint64,
                       const CloudSharedAssetMutationResult& result) {
            itemResults.push_back(result);
        });
    QObject::connect(
        &coordinator, &CloudSharedAssetMutationCoordinator::batchReady,
        [&batchResults](
            quint64,
            const QVector<CloudSharedAssetMutationResult>& results) {
            batchResults = results;
        });
    QObject::connect(
        &coordinator, &CloudSharedAssetMutationCoordinator::batchCancelled,
        [&cancelled](quint64) { ++cancelled; });

    CloudSharedAssetMutationInput wav =
        sharedTestInput(1, QStringLiteral("audio/wav"));
    wav.displayName = QStringLiteral("C:\\private\\sample-1.wav");
    const CloudSharedAssetMutationInput sample =
        sharedTestInput(2, QStringLiteral("audio/flac"));
    const quint64 generation = coordinator.begin({wav, sample});
    if (generation == 0 || port.imports.size() != 2 ||
        !port.uploads.isEmpty() || !itemResults.isEmpty() ||
        !batchResults.isEmpty()) {
        return fail(QStringLiteral(
            "shared assets escaped before cache import and verified upload"));
    }

    port.completeImport(port.imports[0].id);
    if (port.uploads.size() != 1 || !itemResults.isEmpty())
        return fail(QStringLiteral("cache import exposed a shared AssetRef"));
    const FakeSharedAssetPort::UploadCall firstUpload = port.uploads.front();
    if (firstUpload.input.projectId != wav.projectId ||
        firstUpload.input.uploadId != wav.uploadId ||
        firstUpload.input.assetId != wav.assetId ||
        firstUpload.input.sourcePath == wav.sourcePath ||
        firstUpload.input.sha256.size() != 64 ||
        firstUpload.input.byteSize == 0 ||
        firstUpload.input.kind != CloudAssetKind::Audio ||
        firstUpload.input.contentType != wav.contentType ||
        firstUpload.input.displayName != QLatin1String("sample-1.wav")) {
        return fail(QStringLiteral(
            "shared upload did not use stable identities and cached bytes"));
    }
    port.completeUpload(firstUpload.id);
    if (itemResults.size() != 1 || !batchResults.isEmpty() ||
        itemResults.front().cachedLocalPath != firstUpload.input.sourcePath ||
        itemResults.front().asset.assetId != wav.assetId.toStdString() ||
        itemResults.front().asset.sha256.size() != 64 ||
        itemResults.front().asset.byteSize == 0 ||
        itemResults.front().asset.originalName != "sample-1.wav" ||
        itemResults.front().asset.originalName.find("private/") !=
            std::string::npos) {
        return fail(QStringLiteral(
            "verified shared asset result leaked or changed local identity"));
    }

    port.completeImport(port.imports[1].id);
    const FakeSharedAssetPort::UploadCall secondUpload = port.uploads.back();
    port.completeUpload(secondUpload.id);
    if (coordinator.state() != CloudSharedAssetMutationState::Ready ||
        itemResults.size() != 2 || batchResults.size() != 2 ||
        batchResults[1].asset.mimeType != "audio/flac" ||
        batchResults[1].asset.codec != "flac") {
        return fail(QStringLiteral(
            "verified WAV/Sampler batch did not complete atomically"));
    }

    const CloudSharedAssetMutationInput stale =
        sharedTestInput(3, QStringLiteral("audio/wav"));
    const CloudSharedAssetMutationInput current =
        sharedTestInput(4, QStringLiteral("audio/wav"));
    coordinator.begin({stale});
    const quint64 staleImport = port.imports.back().id;
    coordinator.begin({current});
    const quint64 currentImport = port.imports.back().id;
    const qsizetype uploadsBeforeLateCallback = port.uploads.size();
    port.completeImport(staleImport);
    if (port.uploads.size() != uploadsBeforeLateCallback)
        return fail(QStringLiteral("stale asset generation started an upload"));
    port.completeImport(currentImport);
    const quint64 currentTransfer = port.uploads.back().id;
    port.failUpload(currentTransfer);
    if (!coordinator.retry(current.uploadId) ||
        port.retriedTransfers.isEmpty() ||
        port.retriedTransfers.back() != currentTransfer) {
        return fail(QStringLiteral("shared asset retry lost stable transfer"));
    }
    port.completeUpload(currentTransfer);
    if (coordinator.state() != CloudSharedAssetMutationState::Ready)
        return fail(QStringLiteral("retried shared asset did not become ready"));

    const int cancelledBeforeExplicitCancel = cancelled;
    coordinator.begin({sharedTestInput(5, QStringLiteral("audio/wav"))});
    coordinator.cancel();
    if (coordinator.state() != CloudSharedAssetMutationState::Cancelled ||
        port.cancelledImports.isEmpty() ||
        cancelled != cancelledBeforeExplicitCancel + 1) {
        return fail(QStringLiteral("shared asset cancellation was incomplete"));
    }
    return true;
}

} // namespace collab
