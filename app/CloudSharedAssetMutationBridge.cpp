#include "CloudSharedAssetMutationBridge.hpp"

#include "CloudSharedAssetMutationCoordinator.hpp"
#include "CollaborationService.hpp"
#include "EngineController.hpp"

#include <QUuid>

namespace collab {
namespace {

QString canonicalUuid(const QString& value) {
    const QUuid parsed(value);
    return parsed.isNull()
               ? QString()
               : parsed.toString(QUuid::WithoutBraces).toLower();
}

} // namespace

CloudSharedAssetMutationBridge::CloudSharedAssetMutationBridge(
    CloudSharedAssetMutationCoordinator* coordinator,
    CollaborationService* service, daw::EngineController* controller,
    QObject* parent)
    : QObject(parent),
      m_coordinator(coordinator),
      m_service(service),
      m_controller(controller) {
    if (m_controller) m_controller->attachSharedAssetMutationSink(*this);
    if (m_coordinator) {
        connect(m_coordinator,
                &CloudSharedAssetMutationCoordinator::progressChanged, this,
                [this](quint64 generation, qsizetype completed,
                       qsizetype total) {
                    if (generation == m_generation && active())
                        emit progressChanged(completed, total);
                });
        connect(m_coordinator,
                &CloudSharedAssetMutationCoordinator::batchReady, this,
                &CloudSharedAssetMutationBridge::handleBatchReady);
        connect(m_coordinator,
                &CloudSharedAssetMutationCoordinator::batchFailed, this,
                &CloudSharedAssetMutationBridge::handleBatchFailed);
        connect(m_coordinator,
                &CloudSharedAssetMutationCoordinator::batchCancelled, this,
                &CloudSharedAssetMutationBridge::handleBatchCancelled);
        connect(m_coordinator, &QObject::destroyed, this, [this] {
            m_coordinator = nullptr;
            invalidateActive(false);
        });
    }
    if (m_service) {
        connect(m_service, &CollaborationService::projectChanged, this,
                [this](const QString& projectId) {
                    if (active() && canonicalUuid(projectId) != m_projectId)
                        cancel();
                });
        connect(m_service, &QObject::destroyed, this, [this] {
            m_service = nullptr;
            cancel();
        });
    }
}

CloudSharedAssetMutationBridge::~CloudSharedAssetMutationBridge() {
    if (m_coordinator) {
        disconnect(m_coordinator, nullptr, this, nullptr);
        m_coordinator->cancel();
    }
    invalidateActive(false);
    discardQueued();
    if (m_controller) {
        m_controller->detachSharedAssetMutationSink(*this);
        m_controller = nullptr;
    }
}

QString CloudSharedAssetMutationBridge::currentProjectId() const {
    return m_service ? canonicalUuid(m_service->projectId()) : QString();
}

daw::collab::SharedMutationResult
CloudSharedAssetMutationBridge::prepare(
    daw::collab::SharedAssetMutationRequest request) {
    if (!m_coordinator || !m_service || !m_controller ||
        !m_service->canSubmitOperations()) {
        return daw::collab::SharedMutationResult::Blocked;
    }
    const QString projectId = currentProjectId();
    if (projectId.isEmpty())
        return daw::collab::SharedMutationResult::Blocked;

    CloudSharedAssetMutationInput input;
    input.projectId = projectId;
    input.uploadId = QString::fromStdString(request.requestId);
    input.assetId = QString::fromStdString(request.assetId);
    input.sourcePath = QString::fromStdString(request.sourcePath);
    input.displayName = QString::fromStdString(request.displayName);
    input.contentType = QString::fromStdString(request.contentType);
    input.codec = QString::fromStdString(request.codec);
    input.sampleRate = request.sampleRate;
    input.channels = request.channels;
    input.frames = request.frames;

    // The coordinator owns exactly one generation at a time. Refusing here is
    // what made a multi-file drop import only its first file, so queue instead
    // and start the next one when this batch reaches a terminal state. Every
    // caller still gets Submitted, so every clip appears immediately.
    if (active()) {
        constexpr qsizetype kMaximumQueuedImports = 64;
        if (m_queued.size() >= kMaximumQueuedImports)
            return daw::collab::SharedMutationResult::Blocked;
        m_queued.push_back(input);
        return daw::collab::SharedMutationResult::Submitted;
    }

    m_projectId = projectId;
    m_requestId = input.uploadId;
    m_generation = m_coordinator->generation() + 1;
    if (m_generation == 0) ++m_generation;
    emit activeChanged(true);
    const quint64 started = m_coordinator->begin({input});
    if (started != m_generation) {
        invalidateActive(false);
        return daw::collab::SharedMutationResult::Blocked;
    }
    // A deterministic port may finish during begin(). The terminal handler
    // already submitted/cancelled the retained controller context in that
    // case, so the request was still consumed by the async seam.
    return daw::collab::SharedMutationResult::Submitted;
}

bool CloudSharedAssetMutationBridge::retry() {
    return m_coordinator && active() &&
           m_coordinator->retry(m_requestId);
}

void CloudSharedAssetMutationBridge::cancel() {
    if (!active()) return;
    const QString request = m_requestId;
    if (m_coordinator) m_coordinator->cancel();
    if (m_requestId == request) {
        invalidateActive(false);
        emit cancelled();
    }
}

void CloudSharedAssetMutationBridge::handleBatchReady(
    quint64 generation,
    const QVector<CloudSharedAssetMutationResult>& results) {
    if (generation != m_generation || !active()) return;
    const QString requestId = m_requestId;
    const bool identityMatches =
        results.size() == 1 && results.front().projectId == m_projectId &&
        results.front().uploadId == requestId &&
        currentProjectId() == m_projectId;
    const daw::AssetRef asset =
        identityMatches ? results.front().asset : daw::AssetRef{};
    m_requestId.clear();
    m_projectId.clear();
    m_generation = 0;
    emit activeChanged(false);
    const bool submitted = identityMatches && m_controller &&
        m_controller->completeSharedAssetMutation(
            requestId.toStdString(), asset) ==
            daw::collab::SharedMutationResult::Submitted;
    if (!identityMatches && m_controller)
        m_controller->cancelSharedAssetMutation(requestId.toStdString());
    emit completed(submitted);
    startNextQueued();
}

void CloudSharedAssetMutationBridge::handleBatchFailed(
    quint64 generation, const QString& safeMessage, bool retryable) {
    if (generation != m_generation || !active()) return;
    emit failed(safeMessage, retryable);
    // A retryable failure keeps this request current so Retry can pick it up;
    // the queue waits behind it. A permanent one moves on.
    if (!retryable) {
        invalidateActive(false);
        startNextQueued();
    }
}

void CloudSharedAssetMutationBridge::handleBatchCancelled(
    quint64 generation) {
    if (generation != m_generation || !active()) return;
    invalidateActive(false);
    // Cancelling an import cancels the ones queued behind it too, rather than
    // silently promoting the next file the user did not ask about.
    discardQueued();
    emit cancelled();
}

bool CloudSharedAssetMutationBridge::startNextQueued() {
    while (!m_queued.isEmpty()) {
        const CloudSharedAssetMutationInput input = m_queued.takeFirst();
        const QString projectId = currentProjectId();
        // A queued import belongs to the project it was dropped into. If the
        // binding moved on, retire it instead of pushing it somewhere else.
        if (!m_coordinator || !m_service || !m_controller ||
            projectId.isEmpty() || projectId != input.projectId ||
            !m_service->canSubmitOperations()) {
            if (m_controller) {
                m_controller->cancelSharedAssetMutation(
                    input.uploadId.toStdString());
            }
            continue;
        }
        m_projectId = input.projectId;
        m_requestId = input.uploadId;
        m_generation = m_coordinator->generation() + 1;
        if (m_generation == 0) ++m_generation;
        emit activeChanged(true);
        if (m_coordinator->begin({input}) == m_generation) return true;
        invalidateActive(false);
    }
    return false;
}

void CloudSharedAssetMutationBridge::discardQueued() {
    const QList<CloudSharedAssetMutationInput> queued = std::move(m_queued);
    m_queued.clear();
    for (const CloudSharedAssetMutationInput& input : queued) {
        if (m_controller)
            m_controller->cancelSharedAssetMutation(
                input.uploadId.toStdString());
    }
}

void CloudSharedAssetMutationBridge::invalidateActive(bool notify) {
    if (m_requestId.isEmpty()) return;
    const std::string requestId = m_requestId.toStdString();
    m_requestId.clear();
    m_projectId.clear();
    m_generation = 0;
    emit activeChanged(false);
    if (m_controller)
        m_controller->cancelSharedAssetMutation(requestId);
    if (notify) emit cancelled();
}

} // namespace collab
