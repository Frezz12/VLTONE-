#include "CloudProjectVersionCoordinator.hpp"

#include "AssetCache.hpp"
#include "CloudProjectClient.hpp"
#include "CloudProjectSyncCoordinator.hpp"
#include "CollaborationCommandBridge.hpp"
#include "CollaborationService.hpp"
#include "cloud/PublishPreflight.hpp"

#include <QRegularExpression>
#include <algorithm>

namespace collab {
namespace {
bool verifyMigrationProof(const daw::ProjectModel& document,
    const daw::collab::PluginReadinessReport& report, AssetCache* cache, bool allowExternal) {
    if (!report.ready()) return false;
    const auto preflight = daw::cloud::inspectForPublishV1(document);
    if (!preflight.canPublish()) return false;
    for (const auto& asset : preflight.referencedAssets)
        if (!cache || !cache->contains(asset)) return false;
    const auto requirements = daw::collab::collectPluginRequirements(document);
    for (const auto& requirement : requirements) {
        if (!allowExternal && requirement.format != daw::PluginFormat::Internal) return false;
        const auto match = std::find_if(report.plugins.begin(), report.plugins.end(), [&](const auto& result) {
            return result.requirement == requirement && result.status == daw::collab::PluginReadinessStatus::Ready;
        });
        if (match == report.plugins.end()) return false;
    }
    return true;
}
} // namespace

CloudProjectVersionCoordinator::CloudProjectVersionCoordinator(
    CloudProjectClient* projects, CloudProjectSyncCoordinator* synchronizer,
    CollaborationService* service, CollaborationCommandBridge* bridge,
    AssetCache* cache, QObject* parent)
    : QObject(parent), m_projects(projects), m_sync(synchronizer),
      m_service(service), m_bridge(bridge), m_cache(cache) {
    if (!projects) return;
    connect(projects, &CloudProjectClient::projectReceived, this,
        [this](quint64 request, CloudRequestKind kind, const CloudProjectView& view) {
            if (!m_completion || request != m_request) return;
            m_request = 0;
            if (!proofStillCurrent() || view.project.id != m_projectId ||
                view.role != CloudProjectRole::Owner || view.project.status != CloudProjectStatus::Active ||
                view.project.headSequence != m_head) {
                finish(false, tr("The project changed during its version check. Reopen it before starting.")); return;
            }
            if (kind == CloudRequestKind::MigrateProjectVersion) {
                finish(view.project.engineVersion == m_engineVersion && view.project.minimumAppVersion == m_appVersion,
                    tr("The server did not confirm the requested project version.")); return;
            }
            if (kind != CloudRequestKind::GetProject) { finish(false, tr("Unexpected project version response.")); return; }
            if (view.project.engineVersion == m_engineVersion) { finish(true); return; }
            const auto document = m_bridge->confirmedSnapshotAt(m_head);
            if (!document || !verifyMigrationProof(document->project, m_report, m_cache,
                    view.project.pluginPolicy == QLatin1String("external_checked"))) {
                finish(false, tr("Project files and every plugin state must be verified before updating its version.")); return;
            }
            m_request = m_projects->getActiveSession(m_projectId);
            if (!m_request) finish(false, tr("The active session could not be checked."));
        });
    connect(projects, &CloudProjectClient::sessionStateReceived, this,
        [this](quint64 request, CloudRequestKind, const CloudSessionState&) {
            if (m_completion && request == m_request)
                finish(false, tr("End the current collaboration session before updating the project version."));
        });
    connect(projects, &CloudProjectClient::requestFailed, this,
        [this](quint64 request, CloudRequestKind kind, const CloudClientError& error) {
            if (!m_completion || request != m_request) return;
            m_request = 0;
            if (kind == CloudRequestKind::GetActiveSession && error.httpStatus == 404 &&
                error.apiCode == QLatin1String("collaboration_not_found") && proofStillCurrent()) {
                m_request = m_projects->migrateProjectVersion(m_projectId, m_engineVersion, m_appVersion, m_head);
                if (!m_request) finish(false, tr("The project version update could not be started."));
            } else finish(false, error.safeMessage);
        });
    if (service) connect(service, &CollaborationService::projectChanged, this,
        [this](const QString& id) { if (busy() && id != m_projectId) cancel(); });
}

bool CloudProjectVersionCoordinator::proofStillCurrent() const {
    return m_sync && m_service && m_bridge && m_sync->phase() == CloudSyncPhase::Ready &&
        m_sync->generation() == m_generation && m_sync->projectId() == m_projectId &&
        m_service->projectId() == m_projectId && m_bridge->confirmedServerSequence() == m_head &&
        m_bridge->pendingOperationCount() == 0 && !m_bridge->resyncPending();
}
bool CloudProjectVersionCoordinator::ensureCurrent(const QString& projectId,
    const QString& engineVersion, const QString& appVersion,
    const daw::collab::PluginReadinessReport& report, Completion completion) {
    static const QRegularExpression semver(QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?(?:\\+[0-9A-Za-z.-]+)?$"));
    if (busy() || !m_projects || !m_sync || !m_service || !m_bridge || !completion ||
        engineVersion.isEmpty() || engineVersion.size() > 64 || appVersion.size() > 64 ||
        !semver.match(appVersion).hasMatch() || !report.ready()) return false;
    m_projectId = projectId; m_engineVersion = engineVersion; m_appVersion = appVersion;
    m_generation = m_sync->generation(); m_head = m_bridge->confirmedServerSequence();
    if (!proofStillCurrent()) return false;
    m_report = report; m_completion = std::move(completion);
    m_request = m_projects->getProject(projectId);
    if (!m_request) finish(false, tr("Project metadata could not be checked."));
    return true;
}
void CloudProjectVersionCoordinator::finish(bool success, const QString& message) {
    auto completion = std::move(m_completion);
    m_completion = {}; m_request = 0; m_report = {};
    if (completion) completion(success, success ? QString() : message);
}
void CloudProjectVersionCoordinator::cancel() {
    if (m_projects && m_request) m_projects->cancel(m_request);
    finish(false, tr("Project version check was cancelled."));
}

bool checkCloudProjectVersionCoordinatorForTest(QString* error) {
    const auto fail = [error](const char* message) { if (error) *error = QString::fromUtf8(message); return false; };
    daw::ProjectModel document;
    daw::collab::PluginReadinessReport report;
    if (!verifyMigrationProof(document, report, nullptr, false)) return fail("empty verified project cannot migrate");
    report.stayViewer = true;
    if (verifyMigrationProof(document, report, nullptr, false)) return fail("viewer plugin report permits migration");
    report.stayViewer = false;
    daw::TrackModel track;
    track.id = "11111111-1111-4111-8111-111111111111";
    track.instrument.id = "22222222-2222-4222-8222-222222222222";
    track.instrument.format = daw::PluginFormat::Vst3;
    track.instrument.uid = "fixture-external";
    track.instrument.vendor = "Fixture vendor";
    track.instrument.pluginVersion = "1.0";
    document.tracks.push_back(track);
    if (verifyMigrationProof(document, report, nullptr, true)) return fail("missing plugin probe permits migration");
    for (const auto& requirement : daw::collab::collectPluginRequirements(document))
        report.plugins.push_back({requirement, daw::collab::PluginReadinessStatus::Ready, {}});
    if (!verifyMigrationProof(document, report, nullptr, true) ||
        verifyMigrationProof(document, report, nullptr, false)) return fail("migration plugin profile gate is incorrect");
    report.plugins.front().requirement.version = "0.9";
    if (verifyMigrationProof(document, report, nullptr, true)) return fail("stale plugin version proof permits migration");
    return true;
}
} // namespace collab
