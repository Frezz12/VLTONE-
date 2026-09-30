#include "CloudProjectTitleCoordinator.hpp"

#include "CloudProjectClient.hpp"
#include "CloudProjectSyncCoordinator.hpp"
#include "CollaborationCommandBridge.hpp"
#include "CollaborationService.hpp"

#include <QUuid>

namespace collab {
namespace {
using namespace daw::collab;
std::string nameWriter(const SharedProjectDocument& document) {
    const auto found = document.lastWriterByField.find("project:name");
    return found == document.lastWriterByField.end() ? std::string() : found->second;
}
std::optional<ProjectCommand> titleCommand(const QString& projectId,
    const QString& title, const QString& expectedWriter,
    const SharedProjectDocument& document, int schemaVersion) {
    if (title.isEmpty() || document.project.name == title.toStdString() ||
        nameWriter(document) != expectedWriter.toStdString()) return std::nullopt;
    ProjectCommand command;
    command.meta.schemaVersion = std::uint32_t(schemaVersion);
    command.meta.projectId = projectId.toStdString();
    command.meta.operationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    command.meta.transactionId = command.meta.operationId;
    command.meta.baseServerSequence = document.confirmedSequence;
    // A freshly published name has no writer. The v6 server requires the
    // exact base sequence for an unguarded rename, protecting this first edit.
    if (!expectedWriter.isEmpty())
        command.conditions.push_back(FieldWriterIs{"project:name", expectedWriter.toStdString()});
    command.body = SetProjectScalar{ProjectScalar::Name, title.toStdString()};
    return command;
}
} // namespace

CloudProjectTitleCoordinator::CloudProjectTitleCoordinator(
    CloudProjectClient* projects, CloudProjectSyncCoordinator* synchronizer,
    CollaborationService* service, CollaborationCommandBridge* bridge, QObject* parent)
    : QObject(parent), m_sync(synchronizer), m_service(service), m_bridge(bridge) {
    if (!projects || !synchronizer || !service || !bridge) return;
    connect(projects, &CloudProjectClient::bootstrapCompleted, this,
        [this](quint64, const CloudProjectBootstrap& bootstrap) {
            if (!m_sync || m_sync->projectId() != bootstrap.project.id) return;
            clear();
            if (bootstrap.role != CloudProjectRole::Owner ||
                bootstrap.project.status != CloudProjectStatus::Active) return;
            m_projectId = bootstrap.project.id;
            m_title = bootstrap.project.title;
            m_bootstrapHead = bootstrap.headSequence;
            m_generation = m_sync->generation();
            for (const auto& head : bootstrap.fieldHeads)
                if (head.fieldKey == QLatin1String("project:name")) m_expectedWriter = head.operationId;
            setPending(true);
            // The sync coordinator connected to bootstrapCompleted first.
            // A cached/small bootstrap can finish synchronously before this
            // observer receives the metadata signal.
            if (m_sync->phase() == CloudSyncPhase::Ready && m_service &&
                m_service->projectId() == m_projectId) {
                m_verified = true;
                pump();
            }
        });
    connect(synchronizer, &CloudProjectSyncCoordinator::synchronizedProject, this,
        [this](const QString& id, quint64 sequence, const QString&, CloudProjectRole role, CloudProjectStatus) {
            if (!m_sync || id != m_projectId || m_sync->generation() != m_generation ||
                sequence < m_bootstrapHead || role != CloudProjectRole::Owner) return;
            m_verified = true;
            pump();
        });
    connect(synchronizer, &CloudProjectSyncCoordinator::synchronizationFailed, this,
        [this](quint64 generation, const CloudSyncError&) { if (generation == m_generation) clear(); });
    connect(synchronizer, &CloudProjectSyncCoordinator::phaseChanged, this,
        [this](CloudSyncPhase phase) {
            if (phase == CloudSyncPhase::Ready && m_pending && m_sync && m_service &&
                m_sync->generation() == m_generation && m_sync->projectId() == m_projectId &&
                m_service->projectId() == m_projectId) {
                m_verified = true;
                pump();
            }
        });
    connect(service, &CollaborationService::stateChanged, this, [this] { pump(); });
    connect(service, &CollaborationService::sessionControlChanged, this, [this] { pump(); });
    connect(service, &CollaborationService::projectChanged, this,
        [this](const QString& id) { if (id != m_projectId) clear(); });
    connect(bridge, &CollaborationCommandBridge::pendingOperationCountChanged, this, [this] { pump(); });
    connect(bridge, &CollaborationCommandBridge::operationCommitted, this, [this] { pump(); });
    connect(bridge, &CollaborationCommandBridge::operationDurablyObserved, this,
        [this](const QString& id, quint64, bool) { if (id == m_operationId) clear(); });
    connect(bridge, &CollaborationCommandBridge::operationDurabilityFailed, this,
        [this](const QString& id, const QString&, const QString& message) {
            if (id != m_operationId) return;
            clear();
            emit failed(message);
        });
}

void CloudProjectTitleCoordinator::setPending(bool pending) {
    if (m_pending == pending) return;
    m_pending = pending;
    emit pendingChanged(pending);
}
void CloudProjectTitleCoordinator::clear() {
    m_projectId.clear(); m_title.clear(); m_expectedWriter.clear(); m_operationId.clear();
    m_verified = false;
    setPending(false);
}
void CloudProjectTitleCoordinator::pump() {
    if (!m_pending || !m_verified || !m_service || !m_bridge ||
        m_service->projectId() != m_projectId || !m_operationId.isEmpty()) return;
    auto current = m_bridge->confirmedSnapshotAt(m_bridge->confirmedServerSequence());
    if (!current || current->confirmedSequence < m_bootstrapHead) return;
    auto command = titleCommand(m_projectId, m_title, m_expectedWriter, *current,
                                m_service->commandSchemaVersion());
    // A rename committed since the metadata bootstrap is newer evidence.
    if (!command) { clear(); return; }
    if (!m_service->canSubmitOperations() || m_bridge->pendingOperationCount() != 0) return;
    m_operationId = QString::fromStdString(command->meta.operationId);
    const auto result = m_bridge->submitPreparedCommand(std::move(*command), "Rename cloud project");
    if (result != SharedMutationResult::Submitted) {
        m_operationId.clear();
        return;
    }
    if (m_operationId.isEmpty()) return; // Synchronous acknowledgement.
    const auto watch = m_bridge->watchDurableOperation(m_operationId);
    if (watch.code == DurableOperationWatchCode::AlreadyObserved) clear();
    else if (!watch.accepted()) {
        clear();
        emit failed(tr("Cloud project title confirmation is unavailable. Reopen the project to verify its name."));
    }
}

bool checkCloudProjectTitleCoordinatorForTest(QString* error) {
    const auto fail = [error](const char* message) { if (error) *error = QString::fromUtf8(message); return false; };
    SharedProjectDocument document;
    document.project.name = "Snapshot name";
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto command = titleCommand(id, QStringLiteral("Cloud list rename"), {}, document, 6);
    if (!command || !command->conditions.empty() ||
        command->meta.baseServerSequence != document.confirmedSequence ||
        !ProjectReducer::apply(document, *command).accepted() ||
        document.project.name != "Cloud list rename") return fail("cloud metadata rename was not applied");
    if (titleCommand(id, QStringLiteral("Older metadata"), {}, document, 6))
        return fail("stale metadata overwrote a newer title writer");
    auto guarded = titleCommand(id, QStringLiteral("Second rename"),
        QString::fromStdString(command->meta.operationId), document, 6);
    ProjectCommand other = *command;
    other.meta.operationId = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    other.meta.transactionId = other.meta.operationId;
    other.conditions.clear();
    other.body = SetProjectScalar{ProjectScalar::Name, std::string("Concurrent rename")};
    if (!guarded || !ProjectReducer::apply(document, other).accepted() ||
        ProjectReducer::apply(document, *guarded).code != ApplyCode::PreconditionsFailed ||
        document.project.name != "Concurrent rename") return fail("concurrent cloud title precondition failed");
    return true;
}
} // namespace collab
