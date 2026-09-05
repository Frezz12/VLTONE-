#include "CollaborationCommandBridge.hpp"

#include "CollaborationService.hpp"
#include "collaboration/CommandJson.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QVector>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>

namespace collab {
namespace {

using daw::collab::CommandGateway;
using daw::collab::GatewayCode;
using daw::collab::ProjectCommand;

constexpr double kLargestExactJsonInteger = 9007199254740991.0;
constexpr qsizetype kMaximumJournaledCommandBytes = 1024 * 1024;
constexpr qsizetype kMaximumJournaledCommands = 1024;

QString pendingJournalDirectory(const CollaborationService* service,
                                const QString& projectId) {
    if (!service) return {};
    const QString userId = service->accountUserId();
    const QUuid projectUuid(projectId);
    if (userId.isEmpty() || projectUuid.isNull()) return {};
    const QString normalizedProject =
        projectUuid.toString(QUuid::WithoutBraces).toLower();
    return QDir(QStandardPaths::writableLocation(
                    QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("collaboration/pending-v2/%1/%2")
                      .arg(userId, normalizedProject));
}

QString pendingJournalPath(const CollaborationService* service,
                           const QString& projectId,
                           const QString& operationId) {
    const QString directory = pendingJournalDirectory(service, projectId);
    const QUuid operationUuid(operationId);
    if (directory.isEmpty() || operationUuid.isNull()) return {};
    const QString normalizedOperation =
        operationUuid.toString(QUuid::WithoutBraces).toLower();
    return QDir(directory).filePath(normalizedOperation +
                                    QStringLiteral(".json"));
}

qsizetype uniquePendingOperationCount(
    std::span<const ProjectCommand> optimistic,
    const QStringList& journalFiles) {
    std::unordered_set<std::string> operationIds;
    operationIds.reserve(optimistic.size() + std::size_t(journalFiles.size()));
    for (const ProjectCommand& command : optimistic)
        operationIds.insert(command.meta.operationId);
    for (const QString& file : journalFiles)
        operationIds.insert(QFileInfo(file).completeBaseName().toLower().toStdString());
    operationIds.erase(std::string{});
    return qsizetype(operationIds.size());
}

bool persistPendingCommand(const CollaborationService* service,
                           const ProjectCommand& command) {
    // Dependency-injected/state-machine fixtures have no authenticated account
    // namespace. Production cannot reach a writable service in that state;
    // authenticated cloud submits always take the durable path below.
    if (!service || service->accountUserId().isEmpty()) return true;
    const QString projectId = QString::fromStdString(command.meta.projectId);
    const QString operationId =
        QString::fromStdString(command.meta.operationId);
    const QString path = pendingJournalPath(service, projectId, operationId);
    if (path.isEmpty()) return false;
    const QString directory = QFileInfo(path).absolutePath();
    QDir dir(directory);
    if (!dir.exists() && !QDir().mkpath(directory)) return false;
    if (!QFileInfo::exists(path) &&
        dir.entryList({QStringLiteral("*.json")}, QDir::Files).size() >=
            kMaximumJournaledCommands) return false;
    const QByteArray bytes = QByteArray::fromStdString(
        daw::collab::projectCommandToJson(command).dump());
    if (bytes.isEmpty() || bytes.size() > kMaximumJournaledCommandBytes)
        return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit()) return false;
    QFile::setPermissions(path, QFileDevice::ReadOwner |
                                    QFileDevice::WriteOwner);
    return true;
}

bool removePendingCommand(const CollaborationService* service,
                          const QString& projectId,
                          const QString& operationId) {
    const QString path = pendingJournalPath(service, projectId, operationId);
    return path.isEmpty() || !QFileInfo::exists(path) || QFile::remove(path);
}

bool hasExactKeys(const QJsonObject& object,
                  std::initializer_list<const char*> keys) {
    if (object.size() != qsizetype(keys.size())) return false;
    return std::all_of(keys.begin(), keys.end(), [&](const char* key) {
        return object.contains(QString::fromLatin1(key));
    });
}

bool hasOnlyRejectionKeys(const QJsonObject& object) {
    static constexpr std::array allowed{
        "requestMessageId", "opId", "code", "message", "retryable",
        "headSeq"};
    for (auto iterator = object.constBegin(); iterator != object.constEnd();
         ++iterator) {
        const std::string key = iterator.key().toStdString();
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            return false;
    }
    return object.contains(QStringLiteral("requestMessageId")) &&
           object.contains(QStringLiteral("code")) &&
           object.contains(QStringLiteral("message")) &&
           object.contains(QStringLiteral("retryable"));
}

std::optional<quint64> exactUnsigned(const QJsonValue& value,
                                    bool positive) {
    if (!value.isDouble()) return std::nullopt;
    const double number = value.toDouble(-1.0);
    const double minimum = positive ? 1.0 : 0.0;
    if (!std::isfinite(number) || number < minimum ||
        number > kLargestExactJsonInteger || std::floor(number) != number) {
        return std::nullopt;
    }
    return quint64(number);
}

bool uuidString(const QJsonValue& value, QString* result = nullptr) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (!daw::collab::isUuid(text.toStdString())) return false;
    if (result) *result = text.toLower();
    return true;
}

std::optional<ProjectCommand> commandFromQt(const QJsonObject& object,
                                            QString* error) {
    const QByteArray bytes =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    const nlohmann::json value = nlohmann::json::parse(
        bytes.constData(), bytes.constData() + bytes.size(), nullptr, false);
    if (value.is_discarded()) {
        if (error) *error = QStringLiteral("Invalid command JSON");
        return std::nullopt;
    }
    std::string commandError;
    auto command = daw::collab::projectCommandFromJson(value, &commandError);
    if (!command && error)
        *error = QString::fromStdString(commandError);
    return command;
}

std::optional<QJsonObject> commandToQt(const ProjectCommand& command,
                                       QString* error = nullptr) {
    const std::string bytes =
        daw::collab::projectCommandToJson(command).dump();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(bytes), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Could not encode project command");
        return std::nullopt;
    }
    return document.object();
}

bool supportedRejectionCode(const QString& code) {
    static constexpr std::array<const char*, 19> codes{
        "invalid_message", "forbidden", "read_only", "session_inactive",
        "version_mismatch", "stale_precondition", "entity_deleted",
        "asset_incomplete", "lease_required", "lease_conflict",
        "sequence_gap", "operation_id_reused", "conflict", "rate_limited",
        "collaboration_not_enabled", "hash_consensus_required",
        "cloud_recording_disabled", "storage_quota_exceeded",
        "upload_concurrency_exceeded"};
    return std::any_of(codes.begin(), codes.end(), [&](const char* candidate) {
        return code == QLatin1String(candidate);
    });
}

QString safeServerMessage(const QString& message) {
    if (message.trimmed().isEmpty())
        return QStringLiteral("Operation was rejected");
    QString safe = safeDisplayName(message.left(240));
    if (safe == QLatin1String("Participant"))
        safe = QStringLiteral("Operation was rejected");
    return safe;
}

QJsonObject committedPayload(const QString& projectId, quint64 serverSequence,
                             const QJsonObject& command) {
    return {
        {QStringLiteral("projectId"), projectId},
        {QStringLiteral("serverSeq"), double(serverSequence)},
        {QStringLiteral("actorUserId"),
         QStringLiteral("11111111-1111-4111-8111-111111111111")},
        {QStringLiteral("actorDeviceId"),
         QStringLiteral("22222222-2222-4222-8222-222222222222")},
        {QStringLiteral("committedAtMs"), 1000.0},
        {QStringLiteral("command"), command},
    };
}

ProjectCommand scalarCommand(const char* operationId,
                             daw::collab::ProjectScalar field,
                             daw::collab::ScalarValue value) {
    ProjectCommand command;
    command.meta.operationId = operationId;
    command.body = daw::collab::SetProjectScalar{field, std::move(value)};
    return command;
}

std::string freshUuid() {
    return QUuid::createUuid()
        .toString(QUuid::WithoutBraces)
        .toLower()
        .toStdString();
}

QString canonicalProjectId(const QString& projectId) {
    const QString trimmed = projectId.trimmed();
    if (trimmed.isEmpty()) return {};
    const QUuid uuid(trimmed);
    if (!uuid.isNull())
        return uuid.toString(QUuid::WithoutBraces).toLower();
    // A malformed non-empty binding is still a binding: edits must be blocked,
    // never silently redirected into the local-file mutation path.
    return trimmed.left(128).toLower();
}

} // namespace

CollaborationCommandBridge::CollaborationCommandBridge(
    CollaborationService* service, CommandGateway* gateway, QObject* parent)
    : CollaborationCommandBridge(
          gateway,
          [guard = QPointer<CollaborationService>(service)](
              const QJsonObject& command) {
              return guard && guard->submitOperation(command);
          },
          [guard = QPointer<CollaborationService>(service)] {
              return guard && guard->canSubmitOperations();
          },
          [guard = QPointer<CollaborationService>(service)] {
              return guard ? guard->projectId() : QString();
          },
          parent) {
    m_service = service;
    if (!service) return;
    connect(service, &CollaborationService::durableEnvelopeReceived, this,
            &CollaborationCommandBridge::receiveDurableEnvelope);
    connect(service, &CollaborationService::resyncRequired, this,
            [this](const QJsonObject& payload) {
                const quint64 expected =
                    m_gateway ? m_gateway->confirmed().confirmedSequence + 1 : 1;
                const quint64 received =
                    exactUnsigned(payload.value(QStringLiteral("headSeq")),
                                  false)
                        .value_or(0);
                markResync(expected, received,
                           QStringLiteral("Server requested project resync"));
            });
    connect(service, &CollaborationService::projectChanged, this,
            [this](const QString& projectId) {
                applyProjectBinding(projectId);
            });
    connect(service, &QObject::destroyed, this,
            [this] {
                m_service = nullptr;
                applyProjectBinding({});
            });
}

CollaborationCommandBridge::CollaborationCommandBridge(
    CommandGateway* gateway, OperationSender sender,
    AvailabilityCheck available, QObject* parent)
    : CollaborationCommandBridge(gateway, std::move(sender),
                                 std::move(available), ProjectIdProvider{},
                                 parent) {}

CollaborationCommandBridge::CollaborationCommandBridge(
    CommandGateway* gateway, OperationSender sender,
    AvailabilityCheck available, ProjectIdProvider projectId,
    QObject* parent)
    : QObject(parent),
      m_gateway(gateway),
      m_sender(std::move(sender)),
      m_available(std::move(available)),
      m_projectIdProvider(std::move(projectId)) {
    Q_ASSERT(m_gateway);
    refreshProjectBinding();
}

void CollaborationCommandBridge::refreshProjectBinding() {
    applyProjectBinding(m_projectIdProvider ? m_projectIdProvider()
                                            : QString());
}

void CollaborationCommandBridge::applyProjectBinding(
    const QString& projectId) {
    const QString normalized = canonicalProjectId(projectId);
    if (normalized == m_boundProjectId) return;

    // A pending operation belongs to exactly one cloud project. Never replay
    // it into a replacement snapshot after Leave/Open/New changes the binding.
    std::vector<std::string> pendingIds;
    if (m_gateway) {
        pendingIds.reserve(m_gateway->pending().size());
        for (const ProjectCommand& command : m_gateway->pending())
            pendingIds.push_back(command.meta.operationId);
    }
    std::vector<std::string> bindingWatchIds(
        m_watchedDurableOperationIds.begin(),
        m_watchedDurableOperationIds.end());
    std::sort(bindingWatchIds.begin(), bindingWatchIds.end());

    // Detach every old-project watch before invoking a direct Qt slot. The
    // callback is then free to register the same operation id against the new
    // binding without that new watch being consumed by the remainder of this
    // transition. Taking the whole set up front also guarantees one terminal
    // notification per old watch even when a slot changes binding again.
    m_watchedDurableOperationIds.clear();
    m_history.clear();
    clearPendingBookkeeping();
    for (const std::string& operationId : pendingIds) {
        if (m_gateway)
            m_gateway->rejectPending(operationId,
                                     "cloud project binding changed");
    }

    m_boundProjectId = normalized;
    m_verifiedSnapshotProjectId.clear();
    m_resyncPending = false;
    m_deferredCommitted.clear();
    m_deferredCommittedBytes = 0;
    m_deferredOverflow = false;
    clearDurableObservations();
    emitHistoryAvailability();

    for (const std::string& operationId : bindingWatchIds) {
        emit operationDurabilityFailed(
            QString::fromStdString(operationId),
            QStringLiteral("project_binding_changed"),
            QStringLiteral(
                "Cloud project changed before durability was confirmed"));
    }
    if (!pendingIds.empty()) {
        QStringList dropped;
        dropped.reserve(qsizetype(pendingIds.size()));
        for (const std::string& operationId : pendingIds)
            dropped.push_back(QString::fromStdString(operationId));
        emit pendingOperationsDropped(dropped);
    }
}

bool CollaborationCommandBridge::rememberDurableOperation(
    const std::string& operationId, quint64 serverSequence,
    bool viaVerifiedSnapshot) {
    // The live socket and a REST bootstrap can prove the same operation in
    // either order. Keep a bounded recent-id window so their duplicate
    // delivery cannot run recovery cleanup twice. The websocket/deferred
    // queues are bounded well below this window; an ancient replay after
    // eviction may be signalled again, hence the intentionally "exact-once-ish"
    // contract rather than durable local persistence here.
    static constexpr std::size_t kMaximumObservedOperations = 8192;
    if (serverSequence == 0 ||
        !daw::collab::isUuid(operationId) ||
        QUuid(QString::fromStdString(operationId)).isNull() ||
        !m_durableObservations
             .emplace(operationId,
                      DurableOperationWatchResult{
                          DurableOperationWatchCode::AlreadyObserved,
                          serverSequence, viaVerifiedSnapshot})
             .second) {
        return false;
    }
    m_watchedDurableOperationIds.erase(operationId);
    m_durableObservationOrder.push_back(operationId);
    while (m_durableObservationOrder.size() > kMaximumObservedOperations) {
        m_durableObservations.erase(m_durableObservationOrder.front());
        m_durableObservationOrder.pop_front();
    }
    return true;
}

void CollaborationCommandBridge::observeDurableOperation(
    const std::string& operationId, quint64 serverSequence,
    bool viaVerifiedSnapshot) {
    if (!rememberDurableOperation(operationId, serverSequence,
                                  viaVerifiedSnapshot)) {
        return;
    }
    removePendingCommand(m_service, m_boundProjectId,
                         QString::fromStdString(operationId));
    emit operationDurablyObserved(QString::fromStdString(operationId),
                                  serverSequence, viaVerifiedSnapshot);
}

void CollaborationCommandBridge::failDurableOperationWatch(
    const std::string& operationId, const QString& code,
    const QString& safeMessage) {
    if (m_watchedDurableOperationIds.erase(operationId) == 0) return;
    // Erase before emitting: a direct slot may intentionally re-watch for a
    // retry, and the remainder of this terminal path must not fail it twice.
    emit operationDurabilityFailed(QString::fromStdString(operationId), code,
                                   safeMessage);
}

void CollaborationCommandBridge::clearDurableObservations() {
    m_durableObservations.clear();
    m_durableObservationOrder.clear();
    m_watchedDurableOperationIds.clear();
}

DurableOperationWatchResult
CollaborationCommandBridge::watchDurableOperation(
    const QString& operationId) {
    refreshProjectBinding();
    const std::string canonicalId = operationId.toStdString();
    if (!daw::collab::isUuid(canonicalId) || QUuid(operationId).isNull())
        return {DurableOperationWatchCode::InvalidOperationId};
    if (m_boundProjectId.isEmpty())
        return {DurableOperationWatchCode::ProjectUnbound};
    const auto observed = m_durableObservations.find(canonicalId);
    if (observed != m_durableObservations.end())
        return observed->second;
    if (m_gateway && !m_verifiedSnapshotProjectId.isEmpty() &&
        m_verifiedSnapshotProjectId == m_boundProjectId &&
        m_gateway->confirmed().confirmedSequence > 0 &&
        m_gateway->confirmed().appliedOperationIds.contains(canonicalId)) {
        const quint64 confirmedHead =
            m_gateway->confirmed().confirmedSequence;
        rememberDurableOperation(canonicalId, confirmedHead, true);
        return {DurableOperationWatchCode::AlreadyObserved,
                confirmedHead, true};
    }

    static constexpr std::size_t kMaximumWatchedOperations = 8192;
    if (!m_watchedDurableOperationIds.contains(canonicalId) &&
        m_watchedDurableOperationIds.size() >= kMaximumWatchedOperations) {
        return {DurableOperationWatchCode::CapacityExceeded};
    }
    m_watchedDurableOperationIds.insert(canonicalId);
    return {DurableOperationWatchCode::Watching};
}

bool CollaborationCommandBridge::handlesCloudBinding() {
    refreshProjectBinding();
    return !m_boundProjectId.isEmpty();
}

daw::collab::SharedMutationResult CollaborationCommandBridge::submit(
    daw::collab::SharedMutationRequest request) {
    return submitShared(std::move(request.body),
                        std::move(request.undoLabel),
                        std::move(request.transactionId));
}

qsizetype CollaborationCommandBridge::pendingOperationCount() const {
    std::span<const ProjectCommand> optimistic;
    if (m_gateway) optimistic = m_gateway->pending();
    const QString directory = pendingJournalDirectory(m_service, m_boundProjectId);
    const QStringList journalFiles = directory.isEmpty()
        ? QStringList{}
        : QDir(directory).entryList({QStringLiteral("*.json")}, QDir::Files);
    return uniquePendingOperationCount(optimistic, journalFiles);
}

qsizetype CollaborationCommandBridge::journalEntryCount(
    const QString& projectId) const {
    const QString directory = pendingJournalDirectory(m_service, projectId);
    if (directory.isEmpty()) return 0;
    return QDir(directory)
        .entryList({QStringLiteral("*.json")}, QDir::Files)
        .size();
}

QVector<daw::collab::ProjectCommand>
CollaborationCommandBridge::journaledOperations(
    const QString& requestedProjectId) const {
    QVector<daw::collab::ProjectCommand> result;
    const QUuid projectUuid(requestedProjectId);
    if (projectUuid.isNull()) return result;
    const QString projectId =
        projectUuid.toString(QUuid::WithoutBraces).toLower();
    const QString directory = pendingJournalDirectory(m_service, projectId);
    if (directory.isEmpty()) return result;
    const QFileInfoList files = QDir(directory).entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files,
        QDir::Time | QDir::Reversed | QDir::Name);
    result.reserve(std::min<qsizetype>(files.size(),
                                      kMaximumJournaledCommands));
    for (qsizetype index = 0;
         index < files.size() && index < kMaximumJournaledCommands; ++index) {
        QFile file(files.at(index).absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = file.read(kMaximumJournaledCommandBytes + 1);
        if (bytes.isEmpty() || bytes.size() > kMaximumJournaledCommandBytes)
            continue;
        const nlohmann::json json = nlohmann::json::parse(
            bytes.constData(), bytes.constData() + bytes.size(), nullptr,
            false);
        if (json.is_discarded()) continue;
        std::string error;
        auto command = daw::collab::projectCommandFromJson(json, &error);
        if (!command || command->meta.projectId != projectId ||
            QString::fromStdString(command->meta.operationId) !=
                files.at(index).completeBaseName().toLower()) {
            continue;
        }
        result.push_back(std::move(*command));
    }
    return result;
}

bool CollaborationCommandBridge::retireJournaledOperation(
    const QString& projectId, const QString& operationId) {
    const bool removed =
        removePendingCommand(m_service, projectId, operationId);
    if (removed) forgetPending(operationId.toStdString());
    return removed;
}

bool CollaborationCommandBridge::canUndo() {
    refreshProjectBinding();
    return !m_boundProjectId.isEmpty() && m_pendingHistory.empty() &&
           m_history.canUndo();
}

bool CollaborationCommandBridge::canRedo() {
    refreshProjectBinding();
    return !m_boundProjectId.isEmpty() && m_pendingHistory.empty() &&
           m_history.canRedo();
}

void CollaborationCommandBridge::reportPendingDepth() {
    const qsizetype depth = pendingOperationCount();
    if (depth == m_lastPendingDepth) return;
    m_lastPendingDepth = depth;
    emit pendingOperationCountChanged(depth);
}

void CollaborationCommandBridge::emitHistoryAvailability() {
    const bool settled = m_pendingHistory.empty();
    const bool nextUndo = !m_boundProjectId.isEmpty() && settled &&
                          m_history.canUndo();
    const bool nextRedo = !m_boundProjectId.isEmpty() && settled &&
                          m_history.canRedo();
    if (nextUndo != m_lastCanUndo) {
        m_lastCanUndo = nextUndo;
        emit canUndoChanged(nextUndo);
    }
    if (nextRedo != m_lastCanRedo) {
        m_lastCanRedo = nextRedo;
        emit canRedoChanged(nextRedo);
    }
    reportPendingDepth();
}

bool CollaborationCommandBridge::hasPendingHistoryTransition() const {
    return std::any_of(
        m_pendingHistory.cbegin(), m_pendingHistory.cend(),
        [](const auto& entry) {
            return entry.second.kind != PendingHistoryKind::Forward;
        });
}

daw::collab::CommandMeta CollaborationCommandBridge::freshMeta(
    bool transaction) const {
    daw::collab::CommandMeta meta;
    meta.schemaVersion = m_service
        ? std::uint32_t(m_service->commandSchemaVersion())
        : daw::collab::kProjectCommandSchemaVersion;
    meta.projectId = m_boundProjectId.toStdString();
    meta.operationId = freshUuid();
    meta.baseServerSequence = confirmedServerSequence();
    if (transaction) meta.transactionId = freshUuid();
    return meta;
}

daw::collab::SharedMutationResult
CollaborationCommandBridge::submitShared(
    daw::collab::CommandBody body, std::string label,
    std::optional<std::string> transactionId) {
    refreshProjectBinding();
    if (m_boundProjectId.isEmpty())
        return daw::collab::SharedMutationResult::LocalFallback;
    if (m_resyncPending || !m_available || !m_available()) {
        emit mutationBlocked(tr(
            "This cloud project is read-only until the live session is writable."));
        return daw::collab::SharedMutationResult::Blocked;
    }
    if (hasPendingHistoryTransition()) {
        emit mutationBlocked(tr(
            "Wait for cloud undo to finish before making another edit."));
        return daw::collab::SharedMutationResult::Blocked;
    }

    ProjectCommand command;
    command.meta = freshMeta(true);
    if (transactionId) command.meta.transactionId = *transactionId;
    command.body = std::move(body);
    const std::string operationId = command.meta.operationId;
    m_pendingHistory.emplace(
        operationId,
        PendingHistory{PendingHistoryKind::Forward, command, std::move(label),
                       0});
    const LocalOperationResult submitted = submitLocal(std::move(command));
    if (!submitted.submitted()) {
        forgetPending(operationId);
        emit mutationBlocked(tr(
            "The cloud edit could not be submitted. No local change was made."));
        return daw::collab::SharedMutationResult::Blocked;
    }
    emitHistoryAvailability();
    return daw::collab::SharedMutationResult::Submitted;
}

daw::collab::SharedMutationResult
CollaborationCommandBridge::setTimeSignature(int numerator,
                                              int denominator) {
    return submitShared(daw::collab::SetTimeSignature{numerator, denominator},
                        "Set Time Signature");
}

daw::collab::SharedMutationResult CollaborationCommandBridge::setProjectKey(
    int root, std::string_view scaleId) {
    return submitShared(
        daw::collab::SetProjectKey{root, std::string(scaleId)},
        "Set Project Key");
}

daw::collab::SharedMutationResult
CollaborationCommandBridge::setAiInstructions(std::string_view text) {
    return submitShared(
        daw::collab::SetProjectScalar{
            daw::collab::ProjectScalar::AiInstructions, std::string(text)},
        "Edit AI Instructions");
}

daw::collab::SharedMutationResult CollaborationCommandBridge::renameTrack(
    std::string_view trackId, std::string_view name) {
    return submitShared(
        daw::collab::SetTrackProperty{
            std::string(trackId), daw::collab::TrackProperty::Name,
            std::string(name)},
        "Rename Track");
}

daw::collab::SharedMutationResult CollaborationCommandBridge::setTrackMuted(
    std::string_view trackId, bool muted) {
    return submitShared(
        daw::collab::SetTrackProperty{
            std::string(trackId), daw::collab::TrackProperty::Muted, muted},
        muted ? "Mute Track" : "Unmute Track");
}

daw::collab::SharedMutationResult CollaborationCommandBridge::submitMuteBatch(
    std::span<const std::string> trackIds, bool muted, std::string label) {
    refreshProjectBinding();
    if (m_boundProjectId.isEmpty())
        return daw::collab::SharedMutationResult::LocalFallback;
    if (trackIds.empty())
        return daw::collab::SharedMutationResult::Submitted;

    auto batch = std::make_shared<daw::collab::BatchCommand>();
    batch->commands.reserve(trackIds.size());
    std::unordered_set<std::string> seen;
    seen.reserve(trackIds.size());
    for (const std::string& trackId : trackIds) {
        if (trackId.empty() || !seen.insert(trackId).second) continue;
        ProjectCommand child;
        child.body = daw::collab::SetTrackProperty{
            trackId, daw::collab::TrackProperty::Muted, muted};
        batch->commands.push_back(std::move(child));
    }
    if (batch->commands.empty())
        return daw::collab::SharedMutationResult::Submitted;
    const std::string transactionId = freshUuid();
    return submitShared(daw::collab::CommandBody{std::move(batch)},
                        std::move(label), transactionId);
}

daw::collab::SharedMutationResult CollaborationCommandBridge::setTracksMuted(
    std::span<const std::string> trackIds, bool muted) {
    return submitMuteBatch(trackIds, muted,
                           muted ? "Mute Tracks" : "Unmute Tracks");
}

daw::collab::SharedMutationResult CollaborationCommandBridge::clearAllMutes(
    std::span<const std::string> mutedTrackIds) {
    return submitMuteBatch(mutedTrackIds, false, "Clear All Mutes");
}

bool CollaborationCommandBridge::requestUndo() {
    return submitPreparedHistory(false);
}

bool CollaborationCommandBridge::requestRedo() {
    return submitPreparedHistory(true);
}

bool CollaborationCommandBridge::submitPreparedHistory(bool redo) {
    refreshProjectBinding();
    if (m_boundProjectId.isEmpty()) return false;
    if (m_resyncPending || !m_available || !m_available()) {
        emit mutationBlocked(tr(
            "Cloud undo is unavailable until the live session is writable."));
        return false;
    }
    if (!m_pendingHistory.empty()) {
        emit mutationBlocked(tr(
            "Wait for pending cloud edits to synchronize before using undo."));
        return false;
    }

    auto prepared = redo ? m_history.prepareRedo(freshMeta(true))
                         : m_history.prepareUndo(freshMeta(true));
    if (!prepared) return false;
    const std::string operationId = prepared->command.meta.operationId;
    m_pendingHistory.emplace(
        operationId,
        PendingHistory{redo ? PendingHistoryKind::Redo
                            : PendingHistoryKind::Undo,
                       prepared->command, prepared->label, prepared->token});
    emitHistoryAvailability();
    const LocalOperationResult submitted =
        submitLocal(std::move(prepared->command));
    if (!submitted.submitted()) {
        forgetPending(operationId);
        emit mutationBlocked(tr(
            "Cloud undo could not be submitted. The history entry was kept."));
        return false;
    }
    return true;
}

void CollaborationCommandBridge::forgetPending(
    const std::string& operationId) {
    const auto found = m_pendingHistory.find(operationId);
    if (found == m_pendingHistory.end()) return;
    if (found->second.kind != PendingHistoryKind::Forward)
        m_history.cancel(found->second.historyToken);
    m_pendingHistory.erase(found);
    emitHistoryAvailability();
}

void CollaborationCommandBridge::clearPendingBookkeeping() {
    for (const auto& [operationId, pending] : m_pendingHistory) {
        (void)operationId;
        if (pending.kind != PendingHistoryKind::Forward)
            m_history.cancel(pending.historyToken);
    }
    m_pendingHistory.clear();
    emitHistoryAvailability();
}

namespace {
/// Emits the pending-operation depth when the enclosing scope ends, whatever
/// path it leaves by.
struct PendingDepthReporter {
    CollaborationCommandBridge* bridge = nullptr;
    ~PendingDepthReporter() {
        if (bridge) bridge->reportPendingDepth();
    }
};
} // namespace

LocalOperationResult CollaborationCommandBridge::submitLocal(
    ProjectCommand command) {
    return submitLocalImpl(std::move(command), false);
}

daw::collab::SharedMutationResult
CollaborationCommandBridge::submitPreparedCommand(
    ProjectCommand command, std::string undoLabel) {
    refreshProjectBinding();
    if (m_boundProjectId.isEmpty() || !m_service || m_resyncPending || !m_available ||
        !m_available() ||
        command.meta.projectId != m_boundProjectId.toStdString() ||
        command.meta.schemaVersion !=
            std::uint32_t(m_service->commandSchemaVersion())) {
        emit mutationBlocked(tr(
            "The prepared cloud edit no longer matches the writable session."));
        return daw::collab::SharedMutationResult::Blocked;
    }
    const std::string operationId = command.meta.operationId;
    if (m_pendingHistory.contains(operationId)) {
        return daw::collab::SharedMutationResult::Blocked;
    }
    m_pendingHistory.emplace(
        operationId,
        PendingHistory{PendingHistoryKind::Forward, command,
                       std::move(undoLabel), 0});
    const LocalOperationResult submitted = submitLocal(std::move(command));
    if (!submitted.submitted()) {
        forgetPending(operationId);
        return daw::collab::SharedMutationResult::Blocked;
    }
    emitHistoryAvailability();
    return daw::collab::SharedMutationResult::Submitted;
}

LocalOperationResult CollaborationCommandBridge::resubmitJournaled(
    ProjectCommand command) {
    return submitLocalImpl(std::move(command), true);
}

LocalOperationResult CollaborationCommandBridge::submitLocalImpl(
    ProjectCommand command, bool journalRecovery) {
    LocalOperationResult result;
    result.operationId = QString::fromStdString(command.meta.operationId);
    if (!m_gateway) {
        result.message = QStringLiteral("Command gateway is unavailable");
        return result;
    }
    if (m_resyncPending) {
        result.code = LocalOperationCode::ResyncRequired;
        result.message = QStringLiteral("Project resync is required");
        return result;
    }
    const bool available = journalRecovery
        ? m_service && m_service->canSubmitRecoveryOperations()
        : m_available && m_available();
    if (!available) {
        result.code = LocalOperationCode::TransportUnavailable;
        result.message = QStringLiteral("Collaboration session is not writable");
        return result;
    }

    const std::string operationId = command.meta.operationId;
    // Every exit below changes the queue depth, and there are six of them.
    // Reporting on scope exit is what keeps a later added return from silently
    // leaving the status strip showing a stale count.
    const PendingDepthReporter depthReporter{this};
    if (!persistPendingCommand(m_service, command)) {
        result.code = LocalOperationCode::Rejected;
        result.message = QStringLiteral(
            "Pending operation could not be stored safely");
        return result;
    }
    const daw::collab::GatewayUpdate submitted = m_gateway->submit(command);
    if (submitted.code == GatewayCode::Duplicate) {
        result.code = LocalOperationCode::Duplicate;
        result.message = QStringLiteral("Operation was already submitted");
        return result;
    }
    if (submitted.code != GatewayCode::Accepted) {
        removePendingCommand(m_service,
                             QString::fromStdString(command.meta.projectId),
                             QString::fromStdString(operationId));
        result.code = LocalOperationCode::Rejected;
        result.message = QString::fromStdString(submitted.apply.message);
        return result;
    }

    const auto pending = std::find_if(
        m_gateway->pending().begin(), m_gateway->pending().end(),
        [&](const ProjectCommand& candidate) {
            return candidate.meta.operationId == operationId;
        });
    QString encodeError;
    const auto wire = pending == m_gateway->pending().end()
        ? std::optional<QJsonObject>()
        : commandToQt(*pending, &encodeError);
    const bool sent = wire &&
        (journalRecovery
             ? m_service && m_service->submitRecoveryOperation(*wire)
             : m_sender && m_sender(*wire));
    if (!sent) {
        const daw::collab::GatewayUpdate rolledBack =
            m_gateway->rejectPending(operationId,
                                     "operation was not sent");
        reportDropped(rolledBack.droppedPendingOperationIds);
        // rejectPending reports only additional commands that fail replay;
        // the explicitly removed command is not part of that vector.
        failDurableOperationWatch(
            operationId, QStringLiteral("pending_dropped"),
            QStringLiteral(
                "Pending operation was dropped before durability was confirmed"));
        result.code = LocalOperationCode::TransportUnavailable;
        result.message = encodeError.isEmpty()
            ? QStringLiteral("Operation could not be sent")
            : encodeError;
        return result;
    }
    result.code = LocalOperationCode::Submitted;
    return result;
}

void CollaborationCommandBridge::requireResync(
    const QString& safeReason) {
    const quint64 expected =
        m_gateway ? m_gateway->confirmed().confirmedSequence + 1 : 1;
    markResync(expected, 0,
               safeReason.isEmpty()
                   ? QStringLiteral("Project bootstrap is required")
                   : safeReason);
}

void CollaborationCommandBridge::handleProjectionFailure(
    const QString& projectionError) {
    (void)projectionError;
    requireResync(QStringLiteral(
        "Local project materialization failed; refreshing the shared project"));
}

daw::collab::GatewayUpdate
CollaborationCommandBridge::replaceConfirmedSnapshot(
    daw::collab::SharedProjectDocument snapshot, quint64 serverSequence) {
    daw::collab::GatewayUpdate update;
    refreshProjectBinding();
    if (!m_gateway) {
        update.code = GatewayCode::Rejected;
        update.apply.code = daw::collab::ApplyCode::InvalidCommand;
        update.apply.message = "command gateway is unavailable";
        return update;
    }
    if (m_service && !m_service->installVerifiedBootstrapSequence(
                         m_service->projectId(), serverSequence)) {
        update.code = GatewayCode::Rejected;
        update.apply.code = daw::collab::ApplyCode::PreconditionsFailed;
        update.apply.message =
            "verified bootstrap belongs to a stale project";
        return update;
    }

    // A verified snapshot may already contain a local command whose websocket
    // acknowledgement was delayed. Replaying it reports Duplicate, so the
    // generic gateway deliberately keeps it pending. Classify those ids before
    // moving the snapshot, then remove them after installation: otherwise a
    // later stale ack leaves an immortal optimistic command. We intentionally
    // do not reuse the optimistic inverse here: an unseen remote edit may have
    // been ordered immediately before our command, so that inverse can restore
    // the wrong prior value even though its last-writer guard still passes.
    // Only the confirmed reducer ApplyResult is safe enough for actor history.
    std::vector<std::string> incorporatedPending;
    incorporatedPending.reserve(m_gateway->pending().size());
    for (const ProjectCommand& pending : m_gateway->pending()) {
        if (snapshot.appliedOperationIds.contains(pending.meta.operationId))
            incorporatedPending.push_back(pending.meta.operationId);
    }
    std::vector<std::string> incorporatedWatches;
    incorporatedWatches.reserve(m_watchedDurableOperationIds.size());
    for (const std::string& operationId : m_watchedDurableOperationIds) {
        if (snapshot.appliedOperationIds.contains(operationId))
            incorporatedWatches.push_back(operationId);
    }
    std::sort(incorporatedWatches.begin(), incorporatedWatches.end());
    update = m_gateway->replaceConfirmed(std::move(snapshot), serverSequence);
    bool historyUnavailableAfterCatchup = false;
    for (const std::string& operationId : incorporatedPending) {
        const auto bookkeeping = m_pendingHistory.find(operationId);
        historyUnavailableAfterCatchup =
            historyUnavailableAfterCatchup ||
            bookkeeping != m_pendingHistory.end();
        const bool incorporatedHistoryTransition =
            bookkeeping != m_pendingHistory.end() &&
            bookkeeping->second.kind != PendingHistoryKind::Forward;
        const daw::collab::GatewayUpdate removed = m_gateway->rejectPending(
            operationId, "operation is already present in verified snapshot");
        // This queue removal is proof of durability, not failure. Preserve the
        // matching watch until the accepted snapshot is installed below. Any
        // other optimistic command invalidated by replay still fails normally.
        reportDropped(removed.droppedPendingOperationIds, operationId);
        if (incorporatedHistoryTransition) {
            // We know the transition landed, but not the confirmed inverse
            // required to move the stack safely. Clear actor history instead
            // of leaving a stale undo entry aimed at the pre-snapshot writer.
            m_history.clear();
            m_pendingHistory.erase(operationId);
            emitHistoryAvailability();
        } else {
            forgetPending(operationId);
        }
    }
    if (historyUnavailableAfterCatchup) {
        emit protocolWarning(QStringLiteral(
            "Undo history is unavailable for edits confirmed by snapshot catch-up"));
    }
    if (update.accepted()) {
        // Mark provenance before notifying observers. A late watcher may then
        // query this installed canonical state synchronously without confusing
        // an old project's still-materialized gateway document for proof.
        m_verifiedSnapshotProjectId = m_boundProjectId;
        // Each id was pending locally immediately before this verified state
        // was installed, and the canonical snapshot's applied-id set proves
        // it is durable at or before this head. Emit before draining deferred
        // websocket acks so the first proof source remains deterministic.
        for (const std::string& operationId : incorporatedPending)
            observeDurableOperation(operationId, serverSequence, true);
        // A restart can restore cleanup metadata without restoring the
        // gateway's optimistic queue. Only explicitly watched ids are checked;
        // never expand a potentially large snapshot applied-id set into
        // per-operation signals.
        for (const std::string& operationId : incorporatedWatches)
            observeDurableOperation(operationId, serverSequence, true);
        m_resyncPending = false;
        drainDeferredCommitted();
        if (m_deferredOverflow) {
            m_deferredOverflow = false;
            m_deferredCommitted.clear();
            m_deferredCommittedBytes = 0;
            markResync(m_gateway->confirmed().confirmedSequence + 1, 0,
                       QStringLiteral(
                           "Too many operations arrived during project resync"));
        }
        if (!m_resyncPending && m_service)
            m_service->trustedResyncCompleted();
    }
    reportDropped(update.droppedPendingOperationIds);
    return update;
}

std::optional<daw::collab::SharedProjectDocument>
CollaborationCommandBridge::confirmedSnapshotAt(
    quint64 serverSequence) const {
    if (!m_gateway || m_resyncPending ||
        m_gateway->confirmed().confirmedSequence != serverSequence) {
        return std::nullopt;
    }
    return m_gateway->confirmed();
}

quint64 CollaborationCommandBridge::confirmedServerSequence() const noexcept {
    return m_gateway ? m_gateway->confirmed().confirmedSequence : 0;
}

void CollaborationCommandBridge::receiveDurableEnvelope(
    const WireEnvelope& envelope) {
    if (envelope.type == WireType::OpCommitted) {
        if (m_resyncPending) {
            deferCommitted(envelope);
            return;
        }
        receiveCommitted(envelope.payload);
    } else if (envelope.type == WireType::OpRejected) {
        receiveRejected(envelope.payload);
    } else {
        emit protocolWarning(
            QStringLiteral("Unexpected durable collaboration message"));
    }
}

void CollaborationCommandBridge::deferCommitted(
    const WireEnvelope& envelope) {
    static constexpr qsizetype kMaximumDeferredOperations = 4096;
    static constexpr qsizetype kMaximumDeferredBytes = 8 * 1024 * 1024;
    const qsizetype bytes = QJsonDocument(envelope.payload)
                               .toJson(QJsonDocument::Compact)
                               .size();
    if (bytes <= 0 || bytes > kMaximumDeferredBytes ||
        m_deferredCommitted.size() >= kMaximumDeferredOperations ||
        m_deferredCommittedBytes > kMaximumDeferredBytes - bytes) {
        m_deferredOverflow = true;
        m_deferredCommitted.clear();
        m_deferredCommittedBytes = 0;
        return;
    }
    if (!m_deferredOverflow) {
        m_deferredCommitted.push_back(envelope);
        m_deferredCommittedBytes += bytes;
    }
}

void CollaborationCommandBridge::drainDeferredCommitted() {
    QVector<WireEnvelope> deferred = std::move(m_deferredCommitted);
    m_deferredCommitted.clear();
    m_deferredCommittedBytes = 0;
    for (qsizetype index = 0; index < deferred.size(); ++index) {
        if (m_resyncPending) {
            for (; index < deferred.size(); ++index)
                deferCommitted(deferred.at(index));
            return;
        }
        const auto sequence = exactUnsigned(
            deferred.at(index).payload.value(QStringLiteral("serverSeq")),
            true);
        if (sequence && m_gateway &&
            *sequence <= m_gateway->confirmed().confirmedSequence) {
            // The verified snapshot already materializes every operation up
            // to its sequence. Old op ids need not be retained forever merely
            // to recognize broadcasts that were queued before that snapshot.
            continue;
        }
        receiveCommitted(deferred.at(index).payload);
    }
}

void CollaborationCommandBridge::receiveCommitted(const QJsonObject& payload) {
    refreshProjectBinding();
    const quint64 expected =
        m_gateway ? m_gateway->confirmed().confirmedSequence + 1 : 1;
    if (m_resyncPending) return;
    if (!hasExactKeys(payload,
                      {"projectId", "serverSeq", "actorUserId",
                       "actorDeviceId", "committedAtMs", "command"})) {
        emit protocolWarning(QStringLiteral("Invalid committed operation shape"));
        markResync(expected, 0,
                   QStringLiteral("Committed operation could not be decoded"));
        return;
    }
    QString projectId;
    QString actorUserId;
    QString actorDeviceId;
    const auto sequence =
        exactUnsigned(payload.value(QStringLiteral("serverSeq")), true);
    const auto committedAt =
        exactUnsigned(payload.value(QStringLiteral("committedAtMs")), false);
    if (!uuidString(payload.value(QStringLiteral("projectId")), &projectId) ||
        !uuidString(payload.value(QStringLiteral("actorUserId")),
                    &actorUserId) ||
        !uuidString(payload.value(QStringLiteral("actorDeviceId")),
                    &actorDeviceId) ||
        !sequence || !committedAt ||
        !payload.value(QStringLiteral("command")).isObject()) {
        emit protocolWarning(QStringLiteral("Invalid committed operation fields"));
        markResync(expected, sequence.value_or(0),
                   QStringLiteral("Committed operation could not be decoded"));
        return;
    }
    if (!m_boundProjectId.isEmpty() &&
        projectId.compare(m_boundProjectId, Qt::CaseInsensitive) != 0) {
        emit protocolWarning(QStringLiteral("Committed operation project mismatch"));
        markResync(expected, *sequence,
                   QStringLiteral("Committed operation belongs to another project"));
        return;
    }
    if (*sequence > expected) {
        markResync(expected, *sequence,
                   QStringLiteral("Committed operation sequence gap"));
        WireEnvelope deferred;
        deferred.type = WireType::OpCommitted;
        deferred.payload = payload;
        deferCommitted(deferred);
        return;
    }

    QString commandError;
    auto command = commandFromQt(
        payload.value(QStringLiteral("command")).toObject(), &commandError);
    if (!command) {
        emit protocolWarning(QStringLiteral("Invalid committed project command"));
        markResync(expected, *sequence,
                   QStringLiteral("Committed command could not be decoded"));
        return;
    }
    command->meta.projectId = projectId.toStdString();
    command->meta.actorId = actorUserId.toStdString();
    command->meta.clientId = actorDeviceId.toStdString();
    command->meta.serverSequence = *sequence;
    const QString operationId =
        QString::fromStdString(command->meta.operationId);
    const bool localAcknowledgement = std::any_of(
        m_gateway->pending().begin(), m_gateway->pending().end(),
        [&](const ProjectCommand& pending) {
            return pending.meta.operationId == command->meta.operationId;
        });
    const daw::collab::GatewayUpdate update =
        m_gateway->receiveConfirmed(std::move(*command));
    if (update.code == GatewayCode::SequenceGap ||
        update.code == GatewayCode::Rejected) {
        emit protocolWarning(QStringLiteral("Committed command did not converge"));
        markResync(expected, *sequence,
                   QStringLiteral("Committed command requires project resync"));
        return;
    }
    if (m_service && update.code == GatewayCode::Accepted &&
        !m_service->advanceMaterializedSequence(projectId, *sequence)) {
        forgetPending(operationId.toStdString());
        emit protocolWarning(
            QStringLiteral("Materialized collaboration sequence diverged"));
        markResync(expected, *sequence,
                   QStringLiteral("Committed sequence requires project resync"));
        return;
    }
    if (localAcknowledgement) {
        const auto pending = m_pendingHistory.find(
            operationId.toStdString());
        if (pending != m_pendingHistory.end()) {
            if (pending->second.kind == PendingHistoryKind::Forward) {
                m_history.record(pending->second.command, update.apply,
                                 pending->second.label);
            } else {
                m_history.complete(pending->second.historyToken, update.apply);
            }
            m_pendingHistory.erase(pending);
            emitHistoryAvailability();
        }
    }
    reportDropped(update.droppedPendingOperationIds);
    observeDurableOperation(operationId.toStdString(), *sequence, false);
    emit operationCommitted(operationId, *sequence, localAcknowledgement);
}

void CollaborationCommandBridge::receiveRejected(const QJsonObject& payload) {
    if (!hasOnlyRejectionKeys(payload) ||
        !uuidString(payload.value(QStringLiteral("requestMessageId"))) ||
        !payload.value(QStringLiteral("code")).isString() ||
        !payload.value(QStringLiteral("message")).isString() ||
        payload.value(QStringLiteral("message")).toString().size() > 240 ||
        !payload.value(QStringLiteral("retryable")).isBool()) {
        emit protocolWarning(QStringLiteral("Invalid operation rejection"));
        return;
    }
    const QString code = payload.value(QStringLiteral("code")).toString();
    if (!supportedRejectionCode(code)) {
        emit protocolWarning(QStringLiteral("Unknown operation rejection code"));
        return;
    }
    QString operationId;
    if (payload.contains(QStringLiteral("opId")) &&
        !uuidString(payload.value(QStringLiteral("opId")), &operationId)) {
        emit protocolWarning(QStringLiteral("Invalid rejected operation id"));
        return;
    }
    std::optional<quint64> headSequence;
    if (payload.contains(QStringLiteral("headSeq"))) {
        headSequence =
            exactUnsigned(payload.value(QStringLiteral("headSeq")), false);
        if (!headSequence) {
            emit protocolWarning(QStringLiteral("Invalid rejection head sequence"));
            return;
        }
    }
    if (code == QLatin1String("sequence_gap") && !headSequence) {
        emit protocolWarning(
            QStringLiteral("Sequence-gap rejection omitted the server head"));
        return;
    }
    const bool retryable =
        payload.value(QStringLiteral("retryable")).toBool(false);

    if (!operationId.isEmpty() && m_gateway) {
        const daw::collab::GatewayUpdate rejected = m_gateway->rejectPending(
            operationId.toStdString(),
            payload.value(QStringLiteral("message")).toString().toStdString());
        // Keep the explicitly rejected id for the specific terminal signal
        // below. Other pending operations that failed optimistic replay are
        // independent generic drops and must retire their own watches.
        reportDropped(rejected.droppedPendingOperationIds,
                      operationId.toStdString());
        if (!retryable) {
            forgetPending(operationId.toStdString());
            removePendingCommand(m_service, m_boundProjectId, operationId);
        }
    }
    const QString message = safeServerMessage(
        payload.value(QStringLiteral("message")).toString());
    if (!operationId.isEmpty() && !retryable)
        failDurableOperationWatch(operationId.toStdString(), code, message);
    emit operationRejected(operationId, code, message);
    if (retryable || code == QLatin1String("sequence_gap")) {
        const quint64 expected =
            m_gateway ? m_gateway->confirmed().confirmedSequence + 1 : 1;
        markResync(
            expected, headSequence.value_or(0),
            code == QLatin1String("hash_consensus_required")
                ? QStringLiteral(
                      "Server requires project hash consensus before editing")
                : QStringLiteral(
                      "Server requested recovery before retrying an operation"));
    }
}

void CollaborationCommandBridge::markResync(
    quint64 expected, quint64 received, const QString& safeReason) {
    if (m_resyncPending) return;
    m_resyncPending = true;
    if (m_service)
        m_service->trustedResyncRequired(false, false, safeReason);
    emit resyncRequired(expected, received, safeReason);
}

void CollaborationCommandBridge::reportDropped(
    const std::vector<std::string>& operationIds,
    std::string_view durableWatchExclusion) {
    if (operationIds.empty()) return;
    QStringList ids;
    QStringList failedWatchIds;
    ids.reserve(qsizetype(operationIds.size()));
    failedWatchIds.reserve(qsizetype(operationIds.size()));
    for (const std::string& id : operationIds) {
        ids.push_back(QString::fromStdString(id));
        forgetPending(id);
        removePendingCommand(m_service, m_boundProjectId,
                             QString::fromStdString(id));
        if (id != durableWatchExclusion &&
            m_watchedDurableOperationIds.erase(id) > 0) {
            failedWatchIds.push_back(QString::fromStdString(id));
        }
    }
    // All affected watches are removed before the first direct callback. This
    // makes a failure slot that immediately starts a deliberate retry immune
    // to a duplicate id later in the same dropped batch.
    for (const QString& id : failedWatchIds) {
        emit operationDurabilityFailed(
            id, QStringLiteral("pending_dropped"),
            QStringLiteral(
                "Pending operation was dropped before durability was confirmed"));
    }
    emit pendingOperationsDropped(ids);
}

bool checkCollaborationCommandBridgeForTest(QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    struct Adapter final : daw::collab::ProjectProjectionAdapter {
        std::vector<daw::collab::ProjectionOrigin> origins;
        void projectChanged(const daw::collab::SharedProjectDocument&,
                            const daw::collab::ChangeImpact&,
                            daw::collab::ProjectionOrigin origin) override {
            origins.push_back(origin);
        }
    } adapter;

    ProjectCommand firstPending;
    firstPending.meta.operationId =
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    ProjectCommand secondPending;
    secondPending.meta.operationId =
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    const std::array pendingDepthCommands{firstPending, secondPending};
    if (uniquePendingOperationCount(
            pendingDepthCommands,
            {QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa.json"),
             QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc.json")}) != 3) {
        return fail(QStringLiteral(
            "pending operation depth double-counted an optimistic journal entry"));
    }

    CommandGateway gateway({}, &adapter);
    QVector<QJsonObject> sent;
    bool writable = true;
    bool acceptSend = true;
    CollaborationCommandBridge bridge(
        &gateway,
        [&](const QJsonObject& command) {
            if (!acceptSend) return false;
            sent.push_back(command);
            return true;
        },
        [&] { return writable; });
    struct DurableSignal {
        QString operationId;
        quint64 serverSequence = 0;
        bool viaVerifiedSnapshot = false;
    };
    QVector<DurableSignal> durableSignals;
    int resyncSignals = 0;
    int rejectionSignals = 0;
    QObject::connect(&bridge, &CollaborationCommandBridge::resyncRequired,
                     [&](quint64, quint64, const QString&) {
                         ++resyncSignals;
                     });
    QObject::connect(&bridge, &CollaborationCommandBridge::operationRejected,
                     [&](const QString&, const QString&, const QString&) {
                         ++rejectionSignals;
                     });
    QObject::connect(
        &bridge, &CollaborationCommandBridge::operationDurablyObserved,
        [&](const QString& operationId, quint64 serverSequence,
            bool viaVerifiedSnapshot) {
            durableSignals.push_back(
                {operationId, serverSequence, viaVerifiedSnapshot});
        });

    const QString projectId =
        QStringLiteral("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    if (bridge.watchDurableOperation(QStringLiteral(
            "33333333-3333-4333-8333-333333333333")).code !=
        DurableOperationWatchCode::ProjectUnbound) {
        return fail(QStringLiteral(
            "durable watcher escaped an unbound project"));
    }
    ProjectCommand local = scalarCommand(
        "33333333-3333-4333-8333-333333333333",
        daw::collab::ProjectScalar::Tempo, 135.0);
    const LocalOperationResult submitted = bridge.submitLocal(local);
    if (!submitted.submitted() || sent.size() != 1 ||
        gateway.pending().size() != 1 ||
        sent.front().value(QStringLiteral("baseServerSeq")).toDouble(-1) != 0 ||
        sent.front().size() != 8 || !durableSignals.empty()) {
        return fail(QStringLiteral("local command was not submitted optimistically"));
    }
    const LocalOperationResult duplicatePending = bridge.submitLocal(local);
    if (duplicatePending.code != LocalOperationCode::Duplicate ||
        sent.size() != 1 || gateway.pending().size() != 1 ||
        !durableSignals.empty()) {
        return fail(QStringLiteral(
            "duplicate optimistic submit was mistaken for durable proof"));
    }
    QString qtRoundTripError;
    const auto qtRoundTrip = commandFromQt(sent.front(), &qtRoundTripError);
    if (!qtRoundTrip ||
        daw::collab::projectCommandToJson(gateway.pending().front()) !=
            daw::collab::projectCommandToJson(*qtRoundTrip)) {
        return fail(QStringLiteral(
            "Qt numeric command round-trip changed wire semantics"));
    }

    WireEnvelope localAck;
    localAck.type = WireType::OpCommitted;
    localAck.payload = committedPayload(projectId, 1, sent.front());
    const std::size_t projectionsBeforeLocalAck = adapter.origins.size();
    bridge.receiveDurableEnvelope(localAck);
    if (!gateway.pending().empty() ||
        gateway.confirmed().confirmedSequence != 1 ||
        gateway.confirmed().project.tempo != 135.0 ||
        adapter.origins.size() != projectionsBeforeLocalAck ||
        adapter.origins.empty() ||
        adapter.origins.back() !=
            daw::collab::ProjectionOrigin::OptimisticLocal ||
        durableSignals.size() != 1 ||
        durableSignals.front().operationId !=
            QLatin1String("33333333-3333-4333-8333-333333333333") ||
        durableSignals.front().serverSequence != 1 ||
        durableSignals.front().viaVerifiedSnapshot) {
        return fail(QStringLiteral(
            "committed local op did not clear pending state "
            "(pending=%1 seq=%2 tempo=%3 projections=%4 before=%5 last=%6)")
                        .arg(gateway.pending().size())
                        .arg(gateway.confirmed().confirmedSequence)
                        .arg(gateway.confirmed().project.tempo)
                        .arg(adapter.origins.size())
                        .arg(projectionsBeforeLocalAck)
                        .arg(adapter.origins.empty()
                                 ? -1
                                 : int(adapter.origins.back())));
    }

    ProjectCommand remote = scalarCommand(
        "44444444-4444-4444-8444-444444444444",
        daw::collab::ProjectScalar::Name, std::string("Remote"));
    const auto remoteWire = commandToQt(remote);
    if (!remoteWire)
        return fail(QStringLiteral("remote test command did not encode"));
    WireEnvelope remoteCommit;
    remoteCommit.type = WireType::OpCommitted;
    remoteCommit.payload = committedPayload(projectId, 2, *remoteWire);
    bridge.receiveDurableEnvelope(remoteCommit);
    if (gateway.confirmed().confirmedSequence != 2 ||
        gateway.confirmed().project.name != "Remote" || sent.size() != 1 ||
        adapter.origins.back() !=
            daw::collab::ProjectionOrigin::ConfirmedRemote ||
        durableSignals.size() != 2 ||
        durableSignals.back().operationId !=
            QLatin1String("44444444-4444-4444-8444-444444444444") ||
        durableSignals.back().serverSequence != 2 ||
        durableSignals.back().viaVerifiedSnapshot) {
        return fail(QStringLiteral("remote commit echoed or bypassed the gateway"));
    }

    ProjectCommand rejectedLocal = scalarCommand(
        "55555555-5555-4555-8555-555555555555",
        daw::collab::ProjectScalar::MasterPan, 0.5);
    if (!bridge.submitLocal(rejectedLocal).submitted() ||
        gateway.pending().size() != 1 || sent.size() != 2) {
        return fail(QStringLiteral("rejection fixture was not pending"));
    }
    WireEnvelope rejection;
    rejection.type = WireType::OpRejected;
    rejection.payload = {
        {QStringLiteral("requestMessageId"),
         QStringLiteral("66666666-6666-4666-8666-666666666666")},
        {QStringLiteral("opId"),
         QStringLiteral("55555555-5555-4555-8555-555555555555")},
        {QStringLiteral("code"), QStringLiteral("stale_precondition")},
        {QStringLiteral("message"), QStringLiteral("Changed by another editor")},
        {QStringLiteral("retryable"), false},
        {QStringLiteral("headSeq"), 2.0},
    };
    bridge.receiveDurableEnvelope(rejection);
    if (!gateway.pending().empty() || rejectionSignals != 1 ||
        durableSignals.size() != 2 ||
        gateway.optimistic().project.masterPan !=
            gateway.confirmed().project.masterPan) {
        return fail(QStringLiteral("op rejection did not roll back optimistic state"));
    }

    ProjectCommand gapCommand = scalarCommand(
        "77777777-7777-4777-8777-777777777777",
        daw::collab::ProjectScalar::MasterVolume, 0.75);
    const auto gapWire = commandToQt(gapCommand);
    if (!gapWire)
        return fail(QStringLiteral("gap test command did not encode"));
    WireEnvelope gap;
    gap.type = WireType::OpCommitted;
    gap.payload = committedPayload(projectId, 4, *gapWire);
    bridge.receiveDurableEnvelope(gap);
    if (!bridge.resyncPending() || resyncSignals != 1 ||
        gateway.confirmed().confirmedSequence != 2 ||
        durableSignals.size() != 2) {
        return fail(QStringLiteral("server sequence gap did not latch resync"));
    }
    ProjectCommand blocked = scalarCommand(
        "88888888-8888-4888-8888-888888888888",
        daw::collab::ProjectScalar::Tempo, 140.0);
    if (bridge.submitLocal(blocked).code !=
            LocalOperationCode::ResyncRequired ||
        sent.size() != 2) {
        return fail(QStringLiteral("local edit escaped a pending resync"));
    }

    daw::collab::SharedProjectDocument snapshot = gateway.confirmed();
    bridge.replaceConfirmedSnapshot(std::move(snapshot), 3);
    bridge.receiveDurableEnvelope(gap);
    if (bridge.resyncPending() ||
        gateway.confirmed().confirmedSequence != 4 ||
        gateway.confirmed().project.masterVolume != 0.75 || sent.size() != 2 ||
        durableSignals.size() != 3 ||
        durableSignals.back().operationId !=
            QLatin1String("77777777-7777-4777-8777-777777777777") ||
        durableSignals.back().serverSequence != 4 ||
        durableSignals.back().viaVerifiedSnapshot) {
        return fail(QStringLiteral("snapshot did not resume the ordered stream"));
    }
    bridge.receiveDurableEnvelope(gap); // Idempotent redelivery.
    if (bridge.resyncPending() ||
        gateway.confirmed().confirmedSequence != 4 ||
        durableSignals.size() != 3) {
        return fail(QStringLiteral("duplicate committed op triggered resync"));
    }

    acceptSend = false;
    ProjectCommand failedSend = scalarCommand(
        "99999999-9999-4999-8999-999999999999",
        daw::collab::ProjectScalar::Tempo, 145.0);
    if (bridge.submitLocal(failedSend).code !=
            LocalOperationCode::TransportUnavailable ||
        !gateway.pending().empty() ||
        durableSignals.size() != 3 ||
        gateway.optimistic().project.tempo !=
            gateway.confirmed().project.tempo) {
        return fail(QStringLiteral("failed transport send left optimistic state"));
    }
    writable = false;
    acceptSend = true;
    ProjectCommand unavailable = scalarCommand(
        "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
        daw::collab::ProjectScalar::Tempo, 150.0);
    if (bridge.submitLocal(unavailable).code !=
            LocalOperationCode::TransportUnavailable ||
        !gateway.pending().empty() || durableSignals.size() != 3) {
        return fail(QStringLiteral("read-only transport accepted a local command"));
    }

    // Exercise the production service/bridge coupling as a state machine. A
    // sequence gap must block every submit route, not just submitLocal(), and
    // only a verified snapshot may restore the still-connected room.
    CollaborationService service(nullptr);
    service.m_projectId = projectId;
    service.m_shouldConnect = true;
    service.m_transportConnected = true;
    service.m_state = CollaborationState::Synced;
    CommandGateway serviceGateway;
    CollaborationCommandBridge serviceBridge(&service, &serviceGateway);
    int serviceOutbound = 0;
    QObject::connect(&service, &CollaborationService::outboundTextMessage,
                     [&](const QString&) { ++serviceOutbound; });
    const QString serviceSessionId =
        QStringLiteral("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    service.m_sessionId = serviceSessionId;
    const auto verifyServiceRound =
        [&](const QString& roundId, quint64 serverSequence) {
        service.m_bootstrapStateHash = QString(64, QLatin1Char('a'));
        WireEnvelope requested;
        requested.type = WireType::HashRequested;
        requested.payload = {
            {QStringLiteral("roundId"), roundId},
            {QStringLiteral("sessionId"), serviceSessionId},
            {QStringLiteral("serverSeq"), double(serverSequence)},
            {QStringLiteral("deadlineMs"), 4102444800000.0},
        };
        service.handleEnvelope(requested);
        if (service.state() != CollaborationState::Joining ||
            service.canSubmitOperations()) return false;
        WireEnvelope verified;
        verified.type = WireType::HashVerified;
        verified.payload = {
            {QStringLiteral("roundId"), roundId},
            {QStringLiteral("serverSeq"), double(serverSequence)},
        };
        service.handleEnvelope(verified);
        return service.state() == CollaborationState::Synced &&
               service.canSubmitOperations();
    };

    ProjectCommand serviceLocal = scalarCommand(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        daw::collab::ProjectScalar::Tempo, 151.0);
    if (!serviceBridge.submitLocal(serviceLocal).submitted() ||
        serviceOutbound != 1 || serviceGateway.pending().size() != 1) {
        return fail(QStringLiteral("service fixture was not writable before resync"));
    }
    WireEnvelope sequenceRejection;
    sequenceRejection.type = WireType::OpRejected;
    sequenceRejection.payload = {
        {QStringLiteral("requestMessageId"),
         QStringLiteral("cccccccc-cccc-4ccc-8ccc-cccccccccccc")},
        {QStringLiteral("opId"),
         QStringLiteral("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")},
        {QStringLiteral("code"), QStringLiteral("sequence_gap")},
        {QStringLiteral("message"), QStringLiteral("Sequence gap")},
        {QStringLiteral("retryable"), true},
        {QStringLiteral("headSeq"), 5.0},
    };
    service.handleEnvelope(sequenceRejection);
    if (service.state() != CollaborationState::Reconnecting ||
        service.canSubmitOperations() || !serviceBridge.resyncPending() ||
        !serviceGateway.pending().empty()) {
        return fail(QStringLiteral("sequence-gap rejection left service writable"));
    }
    serviceBridge.replaceConfirmedSnapshot(serviceGateway.confirmed(), 5);
    if (service.state() != CollaborationState::Joining ||
        service.canSubmitOperations() || serviceBridge.resyncPending() ||
        !verifyServiceRound(
            QStringLiteral("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"), 5)) {
        return fail(QStringLiteral("verified snapshot did not restore writable state"));
    }

    WireEnvelope welcomeAhead;
    welcomeAhead.type = WireType::Welcome;
    welcomeAhead.payload = {
        {QStringLiteral("projectId"), projectId},
        {QStringLiteral("sessionId"),
         serviceSessionId},
        {QStringLiteral("headSeq"), 6.0},
        {QStringLiteral("readOnly"), false},
        {QStringLiteral("writeBlockedReason"),
         QStringLiteral("hash_consensus_required")},
        {QStringLiteral("hashRound"),
         QJsonObject{
             {QStringLiteral("roundId"),
              QStringLiteral("ffffffff-ffff-4fff-8fff-ffffffffffff")},
             {QStringLiteral("sessionId"), serviceSessionId},
             {QStringLiteral("serverSeq"), 6.0},
             {QStringLiteral("deadlineMs"), 4102444800000.0},
         }},
        {QStringLiteral("participants"), QJsonArray{}},
        {QStringLiteral("limits"),
         QJsonObject{{QStringLiteral("maxMessageBytes"), 1024 * 1024}}},
    };
    service.handleEnvelope(welcomeAhead);
    if (service.state() != CollaborationState::Reconnecting ||
        service.canSubmitOperations() || !serviceBridge.resyncPending()) {
        return fail(QStringLiteral("welcome head gap left service writable"));
    }
    serviceBridge.replaceConfirmedSnapshot(serviceGateway.confirmed(), 6);
    if (service.bootstrapServerSequence() != 6 ||
        service.state() != CollaborationState::Joining ||
        serviceBridge.resyncPending() ||
        !verifyServiceRound(
            QStringLiteral("ffffffff-ffff-4fff-8fff-ffffffffffff"), 6)) {
        return fail(QStringLiteral("welcome head bootstrap was not installed"));
    }

    WireEnvelope hashMismatch;
    hashMismatch.type = WireType::ResyncRequired;
    hashMismatch.payload = {
        {QStringLiteral("reason"), QStringLiteral("hash_mismatch")},
        {QStringLiteral("snapshotSeq"), 5.0},
        {QStringLiteral("headSeq"), 5.0},
        {QStringLiteral("readOnly"), false},
    };
    service.handleEnvelope(hashMismatch);
    if (service.state() != CollaborationState::Reconnecting ||
        service.canSubmitOperations() || !serviceBridge.resyncPending()) {
        return fail(QStringLiteral("hash mismatch left service writable"));
    }
    serviceBridge.replaceConfirmedSnapshot(serviceGateway.confirmed(), 5);
    if (service.state() != CollaborationState::Joining ||
        service.canSubmitOperations() ||
        !verifyServiceRound(
            QStringLiteral("abababab-abab-4bab-8bab-abababababab"), 5)) {
        return fail(QStringLiteral("hash resync did not restore synced state"));
    }

    WireEnvelope conflict;
    conflict.type = WireType::ResyncRequired;
    conflict.payload = {
        {QStringLiteral("reason"), QStringLiteral("conflict")},
        {QStringLiteral("snapshotSeq"), 5.0},
        {QStringLiteral("headSeq"), 5.0},
        {QStringLiteral("readOnly"), true},
    };
    service.handleEnvelope(conflict);
    if (service.state() != CollaborationState::Conflict ||
        service.canSubmitOperations() || !serviceBridge.resyncPending()) {
        return fail(QStringLiteral("conflict resync did not enforce read-only state"));
    }
    serviceBridge.replaceConfirmedSnapshot(serviceGateway.confirmed(), 5);
    if (service.state() != CollaborationState::ReadOnly ||
        service.canSubmitOperations() || serviceBridge.resyncPending()) {
        return fail(QStringLiteral("conflict snapshot restored write access"));
    }

    service.m_resyncPending = true;
    service.m_transportConnected = false;
    if (service.trustedResyncCompleted()) {
        return fail(QStringLiteral("stale disconnected resync callback was accepted"));
    }

    // The SharedMutationSink runtime has a stricter fallback boundary than
    // submitLocal(): an absent binding preserves legacy local behaviour, while
    // every non-empty cloud binding consumes the edit even if its room is
    // offline/read-only. Only a writable room emits a typed command.
    const QString firstTrackId =
        QStringLiteral("10101010-1010-4010-8010-101010101010");
    const QString secondTrackId =
        QStringLiteral("20202020-2020-4020-8020-202020202020");
    daw::collab::SharedProjectDocument mutationDocument;
    daw::TrackModel firstTrack;
    firstTrack.id = firstTrackId.toStdString();
    firstTrack.name = "First";
    firstTrack.muted = false;
    daw::TrackModel secondTrack;
    secondTrack.id = secondTrackId.toStdString();
    secondTrack.name = "Second";
    secondTrack.muted = true;
    mutationDocument.project.tracks = {firstTrack, secondTrack};

    CommandGateway mutationGateway(std::move(mutationDocument));
    QVector<QJsonObject> mutationSent;
    bool mutationWritable = true;
    QString mutationBinding = projectId;
    CollaborationCommandBridge mutationBridge(
        &mutationGateway,
        [&](const QJsonObject& command) {
            mutationSent.push_back(command);
            return true;
        },
        [&] { return mutationWritable; },
        [&] { return mutationBinding; });
    int blockedNotices = 0;
    QObject::connect(&mutationBridge,
                     &CollaborationCommandBridge::mutationBlocked,
                     [&](const QString& message) {
                         if (!message.isEmpty() &&
                             !message.contains(QLatin1Char('/')))
                             ++blockedNotices;
                     });

    const std::array<std::string, 2> mutedTrackIds{
        firstTrackId.toStdString(), secondTrackId.toStdString()};
    if (mutationBridge.setTimeSignature(7, 8) !=
            daw::collab::SharedMutationResult::Submitted ||
        mutationBridge.setProjectKey(11, "dorian") !=
            daw::collab::SharedMutationResult::Submitted ||
        mutationBridge.setAiInstructions("leave headroom") !=
            daw::collab::SharedMutationResult::Submitted ||
        mutationBridge.renameTrack(firstTrackId.toStdString(), "Lead") !=
            daw::collab::SharedMutationResult::Submitted ||
        mutationBridge.setTrackMuted(firstTrackId.toStdString(), true) !=
            daw::collab::SharedMutationResult::Submitted ||
        mutationBridge.clearAllMutes(mutedTrackIds) !=
            daw::collab::SharedMutationResult::Submitted ||
        mutationSent.size() != 6) {
        return fail(QStringLiteral("shared mutation sink did not submit its typed slice"));
    }

    const auto validFreshEnvelope = [&](const QJsonObject& command) {
        return command.size() == 8 &&
                command.value(QStringLiteral("schemaVersion")).toInt() ==
                    daw::collab::kProjectCommandSchemaVersion &&
               uuidString(command.value(QStringLiteral("opId"))) &&
               uuidString(command.value(QStringLiteral("transactionId"))) &&
               command.value(QStringLiteral("baseServerSeq")).toDouble(-1) ==
                   0.0;
    };
    const QJsonObject timePayload =
        mutationSent.at(0).value(QStringLiteral("payload")).toObject();
    const QJsonObject keyPayload =
        mutationSent.at(1).value(QStringLiteral("payload")).toObject();
    const QJsonObject aiPayload =
        mutationSent.at(2).value(QStringLiteral("payload")).toObject();
    const QJsonObject namePayload =
        mutationSent.at(3).value(QStringLiteral("payload")).toObject();
    const QJsonObject mutePayload =
        mutationSent.at(4).value(QStringLiteral("payload")).toObject();
    const QJsonObject batchPayload =
        mutationSent.at(5).value(QStringLiteral("payload")).toObject();
    const QJsonArray batchChildren =
        batchPayload.value(QStringLiteral("commands")).toArray();
    if (!std::all_of(mutationSent.cbegin(), mutationSent.cend(),
                     validFreshEnvelope) ||
        mutationSent.at(0).value(QStringLiteral("kind")).toString() !=
            QLatin1String("project.setTimeSignature") ||
        timePayload.value(QStringLiteral("numerator")).toInt() != 7 ||
        timePayload.value(QStringLiteral("denominator")).toInt() != 8 ||
        mutationSent.at(1).value(QStringLiteral("kind")).toString() !=
            QLatin1String("project.setKey") ||
        keyPayload.value(QStringLiteral("root")).toInt() != 11 ||
        keyPayload.value(QStringLiteral("scale")).toString() !=
            QLatin1String("dorian") ||
        mutationSent.at(2).value(QStringLiteral("kind")).toString() !=
            QLatin1String("project.setScalar") ||
        aiPayload.value(QStringLiteral("field")).toString() !=
            QLatin1String("aiInstructions") ||
        aiPayload.value(QStringLiteral("value")).toString() !=
            QLatin1String("leave headroom") ||
        mutationSent.at(3).value(QStringLiteral("kind")).toString() !=
            QLatin1String("track.setProperty") ||
        namePayload.value(QStringLiteral("trackId")).toString() != firstTrackId ||
        namePayload.value(QStringLiteral("property")).toString() !=
            QLatin1String("name") ||
        namePayload.value(QStringLiteral("value")).toString() !=
            QLatin1String("Lead") ||
        mutePayload.value(QStringLiteral("trackId")).toString() != firstTrackId ||
        mutePayload.value(QStringLiteral("property")).toString() !=
            QLatin1String("muted") ||
        !mutePayload.value(QStringLiteral("value")).toBool(false) ||
        mutationSent.at(5).value(QStringLiteral("kind")).toString() !=
            QLatin1String("batch") ||
        batchChildren.size() != 2 ||
        batchChildren.at(0).toObject().value(QStringLiteral("kind")).toString() !=
            QLatin1String("track.setProperty") ||
        batchChildren.at(1).toObject().value(QStringLiteral("kind")).toString() !=
            QLatin1String("track.setProperty")) {
        return fail(QStringLiteral("shared mutation command mapping or metadata drifted"));
    }

    // Multi-selection/folder mute is one durable outer batch. The controller
    // normally supplies unique IDs, but the bridge remains a linear-time
    // boundary against duplicate/empty children.
    CommandGateway atomicMuteGateway(mutationGateway.confirmed());
    QVector<QJsonObject> atomicMuteSent;
    CollaborationCommandBridge atomicMuteBridge(
        &atomicMuteGateway,
        [&](const QJsonObject& command) {
            atomicMuteSent.push_back(command);
            return true;
        },
        [] { return true; }, [&] { return projectId; });
    const std::array<std::string, 4> duplicateMuteIds{
        firstTrackId.toStdString(), firstTrackId.toStdString(), std::string{},
        secondTrackId.toStdString()};
    const std::array<std::string, 0> emptyMuteIds{};
    if (atomicMuteBridge.setTracksMuted(duplicateMuteIds, true) !=
            daw::collab::SharedMutationResult::Submitted ||
        atomicMuteBridge.setTracksMuted(emptyMuteIds, false) !=
            daw::collab::SharedMutationResult::Submitted ||
        atomicMuteSent.size() != 1 ||
        !validFreshEnvelope(atomicMuteSent.front()) ||
        atomicMuteSent.front().value(QStringLiteral("kind")).toString() !=
            QLatin1String("batch")) {
        return fail(QStringLiteral(
            "atomic mute did not produce exactly one fresh outer batch"));
    }
    const QJsonArray atomicMuteChildren =
        atomicMuteSent.front()
            .value(QStringLiteral("payload"))
            .toObject()
            .value(QStringLiteral("commands"))
            .toArray();
    if (atomicMuteChildren.size() != 2) {
        return fail(QStringLiteral("atomic mute batch did not deduplicate children"));
    }
    for (qsizetype index = 0; index < atomicMuteChildren.size(); ++index) {
        const QJsonObject child = atomicMuteChildren.at(index).toObject();
        const QJsonObject payload =
            child.value(QStringLiteral("payload")).toObject();
        const QString expectedId = index == 0 ? firstTrackId : secondTrackId;
        if (child.value(QStringLiteral("kind")).toString() !=
                QLatin1String("track.setProperty") ||
            payload.value(QStringLiteral("trackId")).toString() != expectedId ||
            payload.value(QStringLiteral("property")).toString() !=
                QLatin1String("muted") ||
            !payload.value(QStringLiteral("value")).toBool(false)) {
            return fail(QStringLiteral("atomic mute child payload drifted"));
        }
    }

    QVector<DurableSignal> mutationDurableSignals;
    QObject::connect(
        &mutationBridge,
        &CollaborationCommandBridge::operationDurablyObserved,
        [&](const QString& operationId, quint64 serverSequence,
            bool viaVerifiedSnapshot) {
            mutationDurableSignals.push_back(
                {operationId, serverSequence, viaVerifiedSnapshot});
        });
    const QString watchedLiveOperationId =
        mutationSent.front().value(QStringLiteral("opId")).toString();
    const DurableOperationWatchResult firstLiveWatch =
        mutationBridge.watchDurableOperation(watchedLiveOperationId);
    const DurableOperationWatchResult repeatedLiveWatch =
        mutationBridge.watchDurableOperation(watchedLiveOperationId);
    if (firstLiveWatch.code != DurableOperationWatchCode::Watching ||
        repeatedLiveWatch.code != DurableOperationWatchCode::Watching) {
        return fail(QStringLiteral(
            "repeated live durable watch was not idempotent"));
    }

    const auto acknowledgeMutation = [&](qsizetype sentIndex,
                                         quint64 sequence) {
        WireEnvelope ack;
        ack.type = WireType::OpCommitted;
        ack.payload = committedPayload(projectId, sequence,
                                       mutationSent.at(sentIndex));
        mutationBridge.receiveDurableEnvelope(ack);
    };
    for (qsizetype index = 0; index < 6; ++index)
        acknowledgeMutation(index, quint64(index + 1));
    const DurableOperationWatchResult observedLiveWatch =
        mutationBridge.watchDurableOperation(watchedLiveOperationId);
    const qsizetype watchedLiveSignalCount = std::count_if(
        mutationDurableSignals.cbegin(), mutationDurableSignals.cend(),
        [&](const DurableSignal& signal) {
            return signal.operationId == watchedLiveOperationId;
        });
    if (!mutationBridge.canUndo() || mutationBridge.canRedo() ||
        mutationGateway.confirmed().confirmedSequence != 6 ||
        mutationGateway.confirmed().project.timeSigNumerator != 7 ||
        mutationGateway.confirmed().project.keyRoot != 11 ||
        mutationGateway.confirmed().project.aiInstructions != "leave headroom" ||
        mutationGateway.confirmed().project.findTrack(
            firstTrackId.toStdString())->name != "Lead" ||
        mutationGateway.confirmed().project.findTrack(
            secondTrackId.toStdString())->muted ||
        mutationDurableSignals.size() != 6 ||
        watchedLiveSignalCount != 1 ||
        mutationDurableSignals.front().operationId != watchedLiveOperationId ||
        mutationDurableSignals.front().serverSequence != 1 ||
        mutationDurableSignals.front().viaVerifiedSnapshot ||
        observedLiveWatch.code !=
            DurableOperationWatchCode::AlreadyObserved ||
        observedLiveWatch.serverSequence != 1 ||
        observedLiveWatch.viaVerifiedSnapshot) {
        return fail(QStringLiteral("own committed acknowledgements did not record history"));
    }

    // A remote edit is materialized and visible, but cannot become the next
    // item in this actor's history.
    ProjectCommand remoteHistoryProbe = scalarCommand(
        "30303030-3030-4030-8030-303030303030",
        daw::collab::ProjectScalar::Name, std::string("Remote title"));
    const auto remoteHistoryWire = commandToQt(remoteHistoryProbe);
    if (!remoteHistoryWire)
        return fail(QStringLiteral("remote history probe did not encode"));
    WireEnvelope remoteHistoryAck;
    remoteHistoryAck.type = WireType::OpCommitted;
    remoteHistoryAck.payload =
        committedPayload(projectId, 7, *remoteHistoryWire);
    mutationBridge.receiveDurableEnvelope(remoteHistoryAck);
    if (!mutationBridge.requestUndo() || mutationSent.size() != 7 ||
        mutationSent.back().value(QStringLiteral("kind")).toString() !=
            QLatin1String("batch") ||
        mutationBridge.canUndo() || mutationBridge.canRedo()) {
        return fail(QStringLiteral("remote operation entered local undo history"));
    }

    acknowledgeMutation(6, 8);
    if (!mutationBridge.canUndo() || !mutationBridge.canRedo() ||
        !mutationGateway.confirmed().project.findTrack(
            firstTrackId.toStdString())->muted ||
        !mutationGateway.confirmed().project.findTrack(
            secondTrackId.toStdString())->muted ||
        !mutationBridge.requestRedo() || mutationSent.size() != 8) {
        return fail(QStringLiteral("confirmed cloud undo did not enable redo"));
    }
    acknowledgeMutation(7, 9);
    if (!mutationBridge.canUndo() || mutationBridge.canRedo() ||
        mutationGateway.confirmed().project.findTrack(
            firstTrackId.toStdString())->muted ||
        mutationGateway.confirmed().project.findTrack(
            secondTrackId.toStdString())->muted) {
        return fail(QStringLiteral("confirmed cloud redo did not restore history"));
    }

    if (!mutationBridge.requestUndo() || mutationSent.size() != 9)
        return fail(QStringLiteral("stale-precondition undo fixture did not submit"));
    const QString rejectedUndoId =
        mutationSent.back().value(QStringLiteral("opId")).toString();
    WireEnvelope rejectedUndo;
    rejectedUndo.type = WireType::OpRejected;
    rejectedUndo.payload = {
        {QStringLiteral("requestMessageId"),
         QStringLiteral("40404040-4040-4040-8040-404040404040")},
        {QStringLiteral("opId"), rejectedUndoId},
        {QStringLiteral("code"), QStringLiteral("stale_precondition")},
        {QStringLiteral("message"), QStringLiteral("Changed by another editor")},
        {QStringLiteral("retryable"), false},
        {QStringLiteral("headSeq"), 9.0},
    };
    mutationBridge.receiveDurableEnvelope(rejectedUndo);
    if (!mutationBridge.canUndo() || mutationBridge.canRedo() ||
        !mutationBridge.requestUndo() || mutationSent.size() != 10) {
        return fail(QStringLiteral("rejected cloud undo did not keep its history entry"));
    }

    const QString bindingPendingOperationId =
        mutationSent.back().value(QStringLiteral("opId")).toString();
    int bindingPendingTerminalCount = 0;
    QString bindingPendingTerminalCode;
    QStringList bindingDroppedPendingIds;
    QObject::connect(
        &mutationBridge,
        &CollaborationCommandBridge::operationDurabilityFailed,
        [&](const QString& operationId, const QString& code, const QString&) {
            if (operationId != bindingPendingOperationId) return;
            ++bindingPendingTerminalCount;
            bindingPendingTerminalCode = code;
        });
    QObject::connect(
        &mutationBridge,
        &CollaborationCommandBridge::pendingOperationsDropped,
        [&](const QStringList& operationIds) {
            bindingDroppedPendingIds.append(operationIds);
        });
    if (mutationBridge.watchDurableOperation(bindingPendingOperationId).code !=
        DurableOperationWatchCode::Watching) {
        return fail(QStringLiteral(
            "binding-switch pending operation could not be watched"));
    }

    mutationBinding =
        QStringLiteral("50505050-5050-4050-8050-505050505050");
    if (!mutationBridge.handlesCloudBinding() || mutationBridge.canUndo() ||
        mutationBridge.canRedo() || !mutationGateway.pending().empty() ||
        bindingPendingTerminalCount != 1 ||
        bindingPendingTerminalCode !=
            QLatin1String("project_binding_changed") ||
        bindingDroppedPendingIds.count(bindingPendingOperationId) != 1) {
        return fail(QStringLiteral("cloud project switch did not clear history and pending ops"));
    }
    mutationWritable = false;
    const qsizetype sentBeforeBlocked = mutationSent.size();
    if (mutationBridge.setAiInstructions("offline") !=
            daw::collab::SharedMutationResult::Blocked ||
        mutationSent.size() != sentBeforeBlocked || blockedNotices != 1) {
        return fail(QStringLiteral("bound read-only edit escaped into a submit/fallback"));
    }
    mutationBinding.clear();
    if (mutationBridge.setAiInstructions("local") !=
            daw::collab::SharedMutationResult::LocalFallback ||
        mutationSent.size() != sentBeforeBlocked) {
        return fail(QStringLiteral("unbound document did not preserve local fallback"));
    }

    // A bootstrap can overtake an own websocket ack. The snapshot proves the
    // operation is committed, but cannot provide the confirmed inverse needed
    // for safe conditional undo. It must clear pending state without inventing
    // history, and the eventual duplicate ack must stay idempotent.
    CommandGateway snapshotAckGateway;
    QVector<QJsonObject> snapshotAckSent;
    CollaborationCommandBridge snapshotAckBridge(
        &snapshotAckGateway,
        [&](const QJsonObject& command) {
            snapshotAckSent.push_back(command);
            return true;
        },
        [] { return true; }, [&] { return projectId; });
    int snapshotAckHistoryWarnings = 0;
    int snapshotAckDurabilityFailures = 0;
    QVector<DurableSignal> snapshotAckDurableSignals;
    bool snapshotAckSignalFollowedInstall = true;
    QObject::connect(
        &snapshotAckBridge, &CollaborationCommandBridge::protocolWarning,
        [&](const QString& warning) {
            if (warning == QLatin1String(
                    "Undo history is unavailable for edits confirmed by snapshot catch-up")) {
                ++snapshotAckHistoryWarnings;
            }
        });
    QObject::connect(
        &snapshotAckBridge,
        &CollaborationCommandBridge::operationDurablyObserved,
        [&](const QString& operationId, quint64 serverSequence,
            bool viaVerifiedSnapshot) {
            snapshotAckSignalFollowedInstall =
                snapshotAckSignalFollowedInstall &&
                snapshotAckGateway.confirmed().confirmedSequence ==
                    serverSequence &&
                snapshotAckGateway.confirmed().appliedOperationIds.contains(
                    operationId.toStdString());
            snapshotAckDurableSignals.push_back(
                {operationId, serverSequence, viaVerifiedSnapshot});
        });
    QObject::connect(
        &snapshotAckBridge,
        &CollaborationCommandBridge::operationDurabilityFailed,
        [&](const QString&, const QString&, const QString&) {
            ++snapshotAckDurabilityFailures;
        });
    if (snapshotAckBridge.setAiInstructions("included by snapshot") !=
            daw::collab::SharedMutationResult::Submitted ||
        snapshotAckSent.size() != 1 ||
        snapshotAckGateway.pending().size() != 1 ||
        !snapshotAckDurableSignals.empty()) {
        return fail(QStringLiteral("snapshot-ack fixture did not submit"));
    }
    const QString snapshotAckOperationId =
        snapshotAckSent.front().value(QStringLiteral("opId")).toString();
    if (snapshotAckBridge.watchDurableOperation(snapshotAckOperationId).code !=
        DurableOperationWatchCode::Watching) {
        return fail(QStringLiteral(
            "snapshot-ack pending operation could not be watched"));
    }
    daw::collab::SharedProjectDocument includesPending =
        snapshotAckGateway.optimistic();
    const auto installedAckSnapshot = snapshotAckBridge.replaceConfirmedSnapshot(
        std::move(includesPending), 1);
    if (!installedAckSnapshot.accepted() ||
        !snapshotAckGateway.pending().empty() || snapshotAckBridge.canUndo() ||
        snapshotAckBridge.canRedo() ||
        snapshotAckHistoryWarnings != 1 ||
        snapshotAckDurabilityFailures != 0 ||
        !snapshotAckSignalFollowedInstall ||
        snapshotAckDurableSignals.size() != 1 ||
        snapshotAckDurableSignals.front().operationId !=
            snapshotAckOperationId ||
        snapshotAckDurableSignals.front().serverSequence != 1 ||
        !snapshotAckDurableSignals.front().viaVerifiedSnapshot ||
        snapshotAckGateway.confirmed().project.aiInstructions !=
            "included by snapshot") {
        return fail(QStringLiteral(
            "verified snapshot did not safely retire its included pending op"));
    }
    WireEnvelope delayedOwnAck;
    delayedOwnAck.type = WireType::OpCommitted;
    delayedOwnAck.payload = committedPayload(projectId, 1,
                                             snapshotAckSent.front());
    snapshotAckBridge.receiveDurableEnvelope(delayedOwnAck);
    if (snapshotAckBridge.resyncPending() ||
        !snapshotAckGateway.pending().empty() || snapshotAckBridge.canUndo() ||
        snapshotAckBridge.canRedo() ||
        snapshotAckHistoryWarnings != 1 ||
        snapshotAckDurabilityFailures != 0 ||
        snapshotAckDurableSignals.size() != 1 ||
        snapshotAckGateway.confirmed().confirmedSequence != 1) {
        return fail(QStringLiteral(
            "delayed duplicate own ack recreated pending/history state"));
    }

    // A durable watch is one-shot in both directions. Explicit server
    // rejection keeps its validated reason, while a local optimistic rollback
    // gets a stable generic reason. Both retire capacity before notifying a
    // direct subscriber, and duplicate terminal input must remain silent.
    struct DurableFailureSignal {
        QString operationId;
        QString code;
        QString safeMessage;
    };
    CommandGateway terminalWatchGateway;
    QVector<QJsonObject> terminalWatchSent;
    bool terminalWatchSendSucceeds = true;
    CollaborationCommandBridge terminalWatchBridge(
        &terminalWatchGateway,
        [&](const QJsonObject& command) {
            if (!terminalWatchSendSucceeds) return false;
            terminalWatchSent.push_back(command);
            return true;
        },
        [] { return true; }, [&] { return projectId; });
    QVector<DurableFailureSignal> durableFailures;
    QObject::connect(
        &terminalWatchBridge,
        &CollaborationCommandBridge::operationDurabilityFailed,
        [&](const QString& operationId, const QString& code,
            const QString& safeMessage) {
            durableFailures.push_back({operationId, code, safeMessage});
        });

    const QString explicitlyRejectedId =
        QStringLiteral("81818181-8181-4181-8181-818181818181");
    ProjectCommand explicitlyRejected = scalarCommand(
        explicitlyRejectedId.toLatin1().constData(),
        daw::collab::ProjectScalar::Tempo, 121.0);
    if (terminalWatchBridge.watchDurableOperation(explicitlyRejectedId).code !=
            DurableOperationWatchCode::Watching ||
        !terminalWatchBridge.submitLocal(explicitlyRejected).submitted() ||
        terminalWatchGateway.pending().size() != 1) {
        return fail(QStringLiteral(
            "explicit rejection durable-watch fixture did not submit"));
    }
    WireEnvelope terminalRejection;
    terminalRejection.type = WireType::OpRejected;
    terminalRejection.payload = {
        {QStringLiteral("requestMessageId"),
         QStringLiteral("82828282-8282-4282-8282-828282828282")},
        {QStringLiteral("opId"), explicitlyRejectedId},
        {QStringLiteral("code"), QStringLiteral("stale_precondition")},
        {QStringLiteral("message"), QStringLiteral("Changed by another editor")},
        {QStringLiteral("retryable"), false},
        {QStringLiteral("headSeq"), 0.0},
    };
    terminalWatchBridge.receiveDurableEnvelope(terminalRejection);
    terminalWatchBridge.receiveDurableEnvelope(terminalRejection);
    if (!terminalWatchGateway.pending().empty() ||
        durableFailures.size() != 1 ||
        durableFailures.front().operationId != explicitlyRejectedId ||
        durableFailures.front().code != QLatin1String("stale_precondition") ||
        durableFailures.front().safeMessage !=
            QLatin1String("Changed by another editor")) {
        return fail(QStringLiteral(
            "explicit rejection did not terminate its durable watch once"));
    }

    const QString droppedPendingId =
        QStringLiteral("83838383-8383-4383-8383-838383838383");
    ProjectCommand droppedPending = scalarCommand(
        droppedPendingId.toLatin1().constData(),
        daw::collab::ProjectScalar::Tempo, 122.0);
    terminalWatchSendSucceeds = false;
    if (terminalWatchBridge.watchDurableOperation(droppedPendingId).code !=
            DurableOperationWatchCode::Watching ||
        terminalWatchBridge.submitLocal(droppedPending).code !=
            LocalOperationCode::TransportUnavailable ||
        !terminalWatchGateway.pending().empty() ||
        durableFailures.size() != 2 ||
        durableFailures.back().operationId != droppedPendingId ||
        durableFailures.back().code != QLatin1String("pending_dropped") ||
        durableFailures.back().safeMessage.isEmpty()) {
        return fail(QStringLiteral(
            "pending rollback did not terminate its durable watch"));
    }
    WireEnvelope rejectionAfterDrop = terminalRejection;
    rejectionAfterDrop.payload.insert(QStringLiteral("requestMessageId"),
                                      QStringLiteral(
                                          "84848484-8484-4484-8484-848484848484"));
    rejectionAfterDrop.payload.insert(QStringLiteral("opId"), droppedPendingId);
    terminalWatchBridge.receiveDurableEnvelope(rejectionAfterDrop);
    if (durableFailures.size() != 2) {
        return fail(QStringLiteral(
            "pending drop and later rejection emitted two watch terminals"));
    }

    // Exercise the exact production bound instead of relying on private state:
    // fill every slot, reject one watched id, then reuse the released slot.
    const auto capacityOperationId = [](quint64 ordinal) {
        return QStringLiteral("d0000000-0000-4000-8000-%1")
            .arg(static_cast<qulonglong>(ordinal), 12, 16,
                 QLatin1Char('0'));
    };
    static constexpr quint64 kDurableWatchCapacity = 8192;
    for (quint64 ordinal = 1; ordinal <= kDurableWatchCapacity; ++ordinal) {
        if (terminalWatchBridge
                .watchDurableOperation(capacityOperationId(ordinal))
                .code != DurableOperationWatchCode::Watching) {
            return fail(QStringLiteral(
                "durable watch capacity filled before its documented bound"));
        }
    }
    const QString capacityOverflowId =
        capacityOperationId(kDurableWatchCapacity + 1);
    if (terminalWatchBridge.watchDurableOperation(capacityOverflowId).code !=
        DurableOperationWatchCode::CapacityExceeded) {
        return fail(QStringLiteral(
            "durable watch capacity did not reject its first overflow"));
    }
    WireEnvelope capacityRejection;
    capacityRejection.type = WireType::OpRejected;
    capacityRejection.payload = {
        {QStringLiteral("requestMessageId"),
         QStringLiteral("85858585-8585-4585-8585-858585858585")},
        {QStringLiteral("opId"), capacityOperationId(1)},
        {QStringLiteral("code"), QStringLiteral("rate_limited")},
        {QStringLiteral("message"), QStringLiteral("Try again later")},
        {QStringLiteral("retryable"), false},
    };
    terminalWatchBridge.receiveDurableEnvelope(capacityRejection);
    if (durableFailures.size() != 3 ||
        durableFailures.back().operationId != capacityOperationId(1) ||
        terminalWatchBridge.watchDurableOperation(capacityOverflowId).code !=
            DurableOperationWatchCode::Watching) {
        return fail(QStringLiteral(
            "terminal rejection did not release durable watch capacity"));
    }

    // Recovery metadata can survive a restart while CommandGateway::pending()
    // cannot. A bounded watcher lets the next verified bootstrap prove exactly
    // the requested operation without emitting the snapshot's entire applied
    // id set. The synchronous AlreadyObserved result closes the late-register
    // race without emitting the signal twice.
    const QString restartWatchedId =
        QStringLiteral("abababab-abab-4aba-8aba-abababababab");
    const QString clearedByBindingSwitchId =
        QStringLiteral("cdcdcdcd-cdcd-4cdc-8dcd-cdcdcdcdcdcd");
    const QString secondClearedByBindingSwitchId =
        QStringLiteral("dededede-dede-4ede-8ede-dededededede");
    const QString unwatchedSnapshotId =
        QStringLiteral("efefefef-efef-4efe-8efe-efefefefefef");
    QString watcherBinding = projectId;
    CommandGateway watcherGateway;
    CollaborationCommandBridge watcherBridge(
        &watcherGateway, [](const QJsonObject&) { return true; },
        [] { return true; }, [&] { return watcherBinding; });
    QVector<DurableSignal> watcherSignals;
    bool watcherSignalsFollowedInstall = true;
    QObject::connect(
        &watcherBridge,
        &CollaborationCommandBridge::operationDurablyObserved,
        [&](const QString& operationId, quint64 serverSequence,
            bool viaVerifiedSnapshot) {
            watcherSignalsFollowedInstall =
                watcherSignalsFollowedInstall &&
                watcherGateway.confirmed().confirmedSequence ==
                    serverSequence &&
                watcherGateway.confirmed().appliedOperationIds.contains(
                    operationId.toStdString());
            watcherSignals.push_back(
                {operationId, serverSequence, viaVerifiedSnapshot});
        });
    QVector<DurableFailureSignal> bindingWatchFailures;
    std::optional<DurableOperationWatchResult> reentrantBindingWatch;
    QObject::connect(
        &watcherBridge,
        &CollaborationCommandBridge::operationDurabilityFailed,
        [&](const QString& operationId, const QString& code,
            const QString& safeMessage) {
            bindingWatchFailures.push_back(
                {operationId, code, safeMessage});
            if (operationId == clearedByBindingSwitchId &&
                !reentrantBindingWatch) {
                reentrantBindingWatch = watcherBridge.watchDurableOperation(
                    clearedByBindingSwitchId);
            }
        });
    const DurableOperationWatchResult invalidWatcher =
        watcherBridge.watchDurableOperation(restartWatchedId.toUpper());
    const DurableOperationWatchResult nilWatcher =
        watcherBridge.watchDurableOperation(QStringLiteral(
            "00000000-0000-0000-0000-000000000000"));
    const DurableOperationWatchResult restartWatcher =
        watcherBridge.watchDurableOperation(restartWatchedId);
    if (invalidWatcher.code !=
            DurableOperationWatchCode::InvalidOperationId ||
        invalidWatcher.accepted() ||
        nilWatcher.code != DurableOperationWatchCode::InvalidOperationId ||
        nilWatcher.accepted() ||
        restartWatcher.code != DurableOperationWatchCode::Watching ||
        !restartWatcher.accepted() || !watcherSignals.empty()) {
        return fail(QStringLiteral(
            "durable watcher accepted a non-canonical id or failed to register"));
    }
    daw::collab::SharedProjectDocument restartSnapshot;
    restartSnapshot.appliedOperationIds.insert(
        restartWatchedId.toStdString());
    restartSnapshot.appliedOperationIds.insert(
        unwatchedSnapshotId.toStdString());
    if (!watcherBridge.replaceConfirmedSnapshot(
             std::move(restartSnapshot), 17).accepted() ||
        !watcherSignalsFollowedInstall || watcherSignals.size() != 1 ||
        watcherSignals.front().operationId != restartWatchedId ||
        watcherSignals.front().serverSequence != 17 ||
        !watcherSignals.front().viaVerifiedSnapshot) {
        return fail(QStringLiteral(
            "restart watcher did not observe exactly its verified snapshot id"));
    }
    const DurableOperationWatchResult alreadyObserved =
        watcherBridge.watchDurableOperation(restartWatchedId);
    const DurableOperationWatchResult snapshotFirstObservation =
        watcherBridge.watchDurableOperation(unwatchedSnapshotId);
    const DurableOperationWatchResult clearedWatcher =
        watcherBridge.watchDurableOperation(clearedByBindingSwitchId);
    const DurableOperationWatchResult secondClearedWatcher =
        watcherBridge.watchDurableOperation(secondClearedByBindingSwitchId);
    if (alreadyObserved.code !=
            DurableOperationWatchCode::AlreadyObserved ||
        alreadyObserved.serverSequence != 17 ||
        !alreadyObserved.viaVerifiedSnapshot ||
        snapshotFirstObservation.code !=
            DurableOperationWatchCode::AlreadyObserved ||
        snapshotFirstObservation.serverSequence != 17 ||
        !snapshotFirstObservation.viaVerifiedSnapshot ||
        clearedWatcher.code != DurableOperationWatchCode::Watching ||
        secondClearedWatcher.code != DurableOperationWatchCode::Watching ||
        watcherSignals.size() != 1) {
        return fail(QStringLiteral(
            "late durable watcher did not return the existing proof"));
    }

    watcherBinding =
        QStringLiteral("60606060-6060-4060-8060-606060606060");
    if (!watcherBridge.handlesCloudBinding() ||
        bindingWatchFailures.size() != 2 ||
        bindingWatchFailures[0].operationId != clearedByBindingSwitchId ||
        bindingWatchFailures[1].operationId !=
            secondClearedByBindingSwitchId ||
        std::any_of(bindingWatchFailures.cbegin(), bindingWatchFailures.cend(),
                    [](const DurableFailureSignal& signal) {
                        return signal.code !=
                                   QLatin1String("project_binding_changed") ||
                               signal.safeMessage.isEmpty();
                    }) ||
        !reentrantBindingWatch ||
        reentrantBindingWatch->code != DurableOperationWatchCode::Watching) {
        return fail(QStringLiteral(
            "project switch did not terminate every old durable watch once"));
    }
    // Refreshing the already-installed binding must neither repeat an old
    // terminal nor consume the watch registered reentrantly by its direct slot.
    if (!watcherBridge.handlesCloudBinding() ||
        bindingWatchFailures.size() != 2 ||
        watcherBridge.watchDurableOperation(clearedByBindingSwitchId).code !=
            DurableOperationWatchCode::Watching) {
        return fail(QStringLiteral(
            "binding terminal was repeated or consumed its reentrant watch"));
    }
    const DurableOperationWatchResult rewatchedAfterSwitch =
        watcherBridge.watchDurableOperation(restartWatchedId);
    daw::collab::SharedProjectDocument switchedProjectSnapshot;
    switchedProjectSnapshot.appliedOperationIds.insert(
        restartWatchedId.toStdString());
    switchedProjectSnapshot.appliedOperationIds.insert(
        clearedByBindingSwitchId.toStdString());
    if (rewatchedAfterSwitch.code != DurableOperationWatchCode::Watching ||
        !watcherBridge.replaceConfirmedSnapshot(
             std::move(switchedProjectSnapshot), 1).accepted() ||
        watcherSignals.size() != 3 ||
        std::count_if(watcherSignals.cbegin(), watcherSignals.cend(),
                      [&](const DurableSignal& signal) {
                          return signal.operationId == restartWatchedId &&
                                 signal.serverSequence == 1 &&
                                 signal.viaVerifiedSnapshot;
                      }) != 1 ||
        std::count_if(watcherSignals.cbegin(), watcherSignals.cend(),
                      [&](const DurableSignal& signal) {
                          return signal.operationId ==
                                     clearedByBindingSwitchId &&
                                 signal.serverSequence == 1 &&
                                 signal.viaVerifiedSnapshot;
                      }) != 1) {
        return fail(QStringLiteral(
            "project binding switch did not preserve reentrant durable watches"));
    }

    ProjectCommand wrongProjectLive = scalarCommand(
        "34343434-3434-4434-8434-343434343434",
        daw::collab::ProjectScalar::Tempo, 146.0);
    const auto wrongProjectLiveWire = commandToQt(wrongProjectLive);
    if (!wrongProjectLiveWire) {
        return fail(QStringLiteral(
            "cross-project live fixture did not encode"));
    }
    WireEnvelope wrongProjectLiveEnvelope;
    wrongProjectLiveEnvelope.type = WireType::OpCommitted;
    wrongProjectLiveEnvelope.payload = committedPayload(
        projectId, 2, *wrongProjectLiveWire);
    watcherBridge.receiveDurableEnvelope(wrongProjectLiveEnvelope);
    if (!watcherBridge.resyncPending() ||
        watcherGateway.confirmed().confirmedSequence != 1 ||
        watcherSignals.size() != 3) {
        return fail(QStringLiteral(
            "bound live commit from another project produced durable proof"));
    }

    // A rejected bootstrap must neither consume its watch nor advertise
    // durability. The same watched id is emitted only after a valid snapshot
    // has actually been installed.
    const QString rejectedSnapshotId =
        QStringLiteral("12121212-1212-4212-8212-121212121212");
    CollaborationService rejectedSnapshotService(nullptr);
    rejectedSnapshotService.m_projectId = projectId;
    CommandGateway rejectedSnapshotGateway;
    CollaborationCommandBridge rejectedSnapshotBridge(
        &rejectedSnapshotService, &rejectedSnapshotGateway);
    QVector<DurableSignal> rejectedSnapshotSignals;
    QObject::connect(
        &rejectedSnapshotBridge,
        &CollaborationCommandBridge::operationDurablyObserved,
        [&](const QString& operationId, quint64 serverSequence,
            bool viaVerifiedSnapshot) {
            rejectedSnapshotSignals.push_back(
                {operationId, serverSequence, viaVerifiedSnapshot});
        });
    if (rejectedSnapshotBridge.watchDurableOperation(rejectedSnapshotId).code !=
        DurableOperationWatchCode::Watching) {
        return fail(QStringLiteral("rejected snapshot watcher did not register"));
    }
    daw::collab::SharedProjectDocument rejectedSnapshot;
    rejectedSnapshot.appliedOperationIds.insert(
        rejectedSnapshotId.toStdString());
    const daw::collab::GatewayUpdate rejectedSnapshotUpdate =
        rejectedSnapshotBridge.replaceConfirmedSnapshot(
            rejectedSnapshot, 9007199254740992ULL);
    if (rejectedSnapshotUpdate.accepted() ||
        rejectedSnapshotGateway.confirmed().confirmedSequence != 0 ||
        !rejectedSnapshotSignals.empty()) {
        return fail(QStringLiteral(
            "rejected verified snapshot emitted a durable observation"));
    }
    if (!rejectedSnapshotBridge.replaceConfirmedSnapshot(
             std::move(rejectedSnapshot), 1).accepted() ||
        rejectedSnapshotSignals.size() != 1 ||
        rejectedSnapshotSignals.front().operationId != rejectedSnapshotId ||
        rejectedSnapshotSignals.front().serverSequence != 1 ||
        !rejectedSnapshotSignals.front().viaVerifiedSnapshot) {
        return fail(QStringLiteral(
            "valid snapshot did not retain and satisfy the durable watch"));
    }

    CommandGateway snapshotUndoGateway;
    QVector<QJsonObject> snapshotUndoSent;
    CollaborationCommandBridge snapshotUndoBridge(
        &snapshotUndoGateway,
        [&](const QJsonObject& command) {
            snapshotUndoSent.push_back(command);
            return true;
        },
        [] { return true; }, [&] { return projectId; });
    int snapshotUndoHistoryWarnings = 0;
    QObject::connect(
        &snapshotUndoBridge, &CollaborationCommandBridge::protocolWarning,
        [&](const QString& warning) {
            if (warning == QLatin1String(
                    "Undo history is unavailable for edits confirmed by snapshot catch-up")) {
                ++snapshotUndoHistoryWarnings;
            }
        });
    if (snapshotUndoBridge.setAiInstructions("undo before bootstrap") !=
            daw::collab::SharedMutationResult::Submitted ||
        snapshotUndoSent.size() != 1) {
        return fail(QStringLiteral("snapshot-undo fixture did not submit"));
    }
    WireEnvelope snapshotUndoForwardAck;
    snapshotUndoForwardAck.type = WireType::OpCommitted;
    snapshotUndoForwardAck.payload =
        committedPayload(projectId, 1, snapshotUndoSent.front());
    snapshotUndoBridge.receiveDurableEnvelope(snapshotUndoForwardAck);
    if (!snapshotUndoBridge.canUndo() ||
        !snapshotUndoBridge.requestUndo() || snapshotUndoSent.size() != 2) {
        return fail(QStringLiteral("snapshot-undo transition did not start"));
    }
    daw::collab::SharedProjectDocument includesPendingUndo =
        snapshotUndoGateway.optimistic();
    if (!snapshotUndoBridge.replaceConfirmedSnapshot(
             std::move(includesPendingUndo), 2).accepted() ||
        !snapshotUndoGateway.pending().empty() || snapshotUndoBridge.canUndo() ||
        snapshotUndoBridge.canRedo() || snapshotUndoHistoryWarnings != 1) {
        return fail(QStringLiteral(
            "snapshot-incorporated undo did not clear ambiguous actor history"));
    }
    WireEnvelope delayedUndoAck;
    delayedUndoAck.type = WireType::OpCommitted;
    delayedUndoAck.payload =
        committedPayload(projectId, 2, snapshotUndoSent.back());
    snapshotUndoBridge.receiveDurableEnvelope(delayedUndoAck);
    if (snapshotUndoBridge.resyncPending() || snapshotUndoBridge.canUndo() ||
        snapshotUndoBridge.canRedo() ||
        snapshotUndoHistoryWarnings != 1 ||
        snapshotUndoGateway.confirmed().confirmedSequence != 2) {
        return fail(QStringLiteral(
            "delayed snapshot-incorporated undo ack was not idempotent"));
    }

    CommandGateway catchupGateway;
    CollaborationCommandBridge catchupBridge(
        &catchupGateway, [](const QJsonObject&) { return true; },
        [] { return true; });
    ProjectCommand catchupOne = scalarCommand(
        "dddddddd-1111-4111-8111-111111111111",
        daw::collab::ProjectScalar::Tempo, 132.0);
    ProjectCommand catchupTwo = scalarCommand(
        "eeeeeeee-2222-4222-8222-222222222222",
        daw::collab::ProjectScalar::MasterPan, 0.25);
    const auto oneJson = commandToQt(catchupOne);
    const auto twoJson = commandToQt(catchupTwo);
    if (!oneJson || !twoJson)
        return fail(QStringLiteral("could not encode catch-up fixture"));
    WireEnvelope committedTwo;
    committedTwo.type = WireType::OpCommitted;
    committedTwo.payload = committedPayload(projectId, 2, *twoJson);
    catchupBridge.receiveDurableEnvelope(committedTwo);
    WireEnvelope committedOne;
    committedOne.type = WireType::OpCommitted;
    committedOne.payload = committedPayload(projectId, 1, *oneJson);
    catchupBridge.receiveDurableEnvelope(committedOne);
    if (!catchupBridge.resyncPending())
        return fail(QStringLiteral("catch-up gap did not latch resync"));
    daw::collab::SharedProjectDocument atOne;
    catchupOne.meta.projectId = projectId.toStdString();
    catchupOne.meta.actorId =
        "11111111-1111-4111-8111-111111111111";
    catchupOne.meta.clientId =
        "22222222-2222-4222-8222-222222222222";
    catchupOne.meta.serverSequence = 1;
    if (!daw::collab::ProjectReducer::apply(atOne, catchupOne).accepted())
        return fail(QStringLiteral("could not build catch-up snapshot"));
    atOne.confirmedSequence = 1;
    catchupBridge.replaceConfirmedSnapshot(std::move(atOne), 1);
    if (catchupBridge.resyncPending() ||
        catchupGateway.confirmed().confirmedSequence != 2 ||
        catchupGateway.confirmed().project.tempo != 132.0 ||
        catchupGateway.confirmed().project.masterPan != 0.25) {
        return fail(QStringLiteral("deferred committed ops did not catch up"));
    }

    CollaborationService reconnectCursor(nullptr);
    reconnectCursor.m_projectId = projectId;
    const QString canonicalHash(64, QLatin1Char('a'));
    if (!reconnectCursor.installVerifiedBootstrapState(
            projectId, 9, canonicalHash) ||
        reconnectCursor.bootstrapStateHash() != canonicalHash ||
        !reconnectCursor.advanceMaterializedSequence(projectId, 10) ||
        reconnectCursor.bootstrapServerSequence() != 10 ||
        !reconnectCursor.bootstrapStateHash().isEmpty() ||
        reconnectCursor.advanceMaterializedSequence(projectId, 12)) {
        return fail(QStringLiteral("reconnect cursor/hash invariant failed"));
    }

    // Engine projection failures have a direct signal/slot seam into the same
    // resync latch as sequence gaps.  No engine diagnostic is reflected into
    // protocol state, and the service becomes non-writable synchronously.
    CollaborationService projectionFailureService(nullptr);
    projectionFailureService.m_projectId = projectId;
    projectionFailureService.m_shouldConnect = true;
    projectionFailureService.m_transportConnected = true;
    projectionFailureService.m_state = CollaborationState::Synced;
    CommandGateway projectionFailureGateway;
    CollaborationCommandBridge projectionFailureBridge(
        &projectionFailureService, &projectionFailureGateway);
    int projectionFailureResyncs = 0;
    QString projectionFailureReason;
    QObject::connect(
        &projectionFailureBridge,
        &CollaborationCommandBridge::resyncRequired,
        [&](quint64, quint64, const QString& reason) {
            ++projectionFailureResyncs;
            projectionFailureReason = reason;
        });
    projectionFailureBridge.handleProjectionFailure(
        QStringLiteral("unsafe engine detail /private/project/file.vlt"));
    ProjectCommand blockedAfterProjectionFailure = scalarCommand(
        "ffffffff-ffff-4fff-8fff-ffffffffffff",
        daw::collab::ProjectScalar::Tempo, 159.0);
    if (!projectionFailureBridge.resyncPending() ||
        projectionFailureResyncs != 1 ||
        projectionFailureService.state() != CollaborationState::Reconnecting ||
        projectionFailureService.canSubmitOperations() ||
        projectionFailureReason.contains(QStringLiteral("/private/")) ||
        projectionFailureBridge.submitLocal(blockedAfterProjectionFailure).code !=
            LocalOperationCode::ResyncRequired) {
        return fail(QStringLiteral(
            "projection failure did not latch the safe service resync path"));
    }

    // QObject teardown is another binding transition: a recovery subscriber
    // must not be left waiting merely because the service disappeared before
    // its websocket acknowledgement arrived.
    auto* destroyedBindingService = new CollaborationService(nullptr);
    destroyedBindingService->m_projectId = projectId;
    CommandGateway destroyedBindingGateway;
    CollaborationCommandBridge destroyedBindingBridge(
        destroyedBindingService, &destroyedBindingGateway);
    const QString destroyedBindingOperationId =
        QStringLiteral("91919191-9191-4191-8191-919191919191");
    int destroyedBindingTerminals = 0;
    QString destroyedBindingCode;
    QObject::connect(
        &destroyedBindingBridge,
        &CollaborationCommandBridge::operationDurabilityFailed,
        [&](const QString& operationId, const QString& code, const QString&) {
            if (operationId != destroyedBindingOperationId) return;
            ++destroyedBindingTerminals;
            destroyedBindingCode = code;
        });
    if (destroyedBindingBridge
            .watchDurableOperation(destroyedBindingOperationId)
            .code != DurableOperationWatchCode::Watching) {
        delete destroyedBindingService;
        return fail(QStringLiteral(
            "service-destruction durable watch did not register"));
    }
    delete destroyedBindingService;
    if (destroyedBindingTerminals != 1 ||
        destroyedBindingCode != QLatin1String("project_binding_changed") ||
        destroyedBindingBridge
                .watchDurableOperation(destroyedBindingOperationId)
                .code != DurableOperationWatchCode::ProjectUnbound) {
        return fail(QStringLiteral(
            "service destruction did not terminate its durable watch once"));
    }
    return true;
}

} // namespace collab
