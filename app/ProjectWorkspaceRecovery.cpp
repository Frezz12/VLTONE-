#include "MainWindow.hpp"
#include "RecoverySupport.hpp"
#include "MidiInputManager.hpp"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QSet>
#include <QSettings>
#include <QStatusBar>

void MainWindow::appendWorkspaceRecovery(daw::recovery::RecoverySnapshot& snapshot) {
    captureProjectView();
    snapshot.selectedDocument = m_projects.selected.toStdString();
    snapshot.documents.clear();
    const auto selectedSnapshot = snapshot;
    for (const auto& session : m_projects.documents) {
        QJsonObject view = QJsonObject::fromVariantMap(session->view);
        view["cursor"] = session->controller.positionSeconds();
        view["selectedTrack"] = session->selectedTrack;
        view["selectedTracks"] = QJsonArray::fromStringList(session->selectedTracks);
        QJsonArray clips;
        for (const auto& clip : session->selectedClips) clips.append(QJsonObject{{"track", clip.trackId}, {"clip", clip.clipId}});
        view["selectedClips"] = clips;
        const auto viewState = QJsonDocument(view).toJson(QJsonDocument::Compact).toStdString();
        const auto& cached = session->recoveryDocument;
        if (session->controller.documentOnly() && cached &&
            session->recoveryControllerRevision == session->controller.projectRevision() && session->recoveryUiRevision == session->revision &&
            cached->path == session->path.toStdString() && cached->viewState == viewState && cached->dirty == session->dirty &&
            cached->name == session->controller.projectName()) {
            snapshot.documents.push_back(cached); continue;
        }
        auto document = std::make_shared<daw::recovery::WorkspaceRecoveryDocument>();
        document->id = session->id.toStdString(); document->path = session->path.toStdString();
        document->name = session->controller.projectName(); document->dirty = session->dirty;
        document->viewState = viewState;
        document->snapshot = session->id == m_projects.selected ? selectedSnapshot : session->controller.captureRecoverySnapshot(0);
        session->recoveryDocument = document;
        session->recoveryControllerRevision = session->controller.projectRevision(); session->recoveryUiRevision = session->revision;
        snapshot.documents.push_back(std::move(document));
    }
}

bool MainWindow::restoreProjectWorkspace(const daw::recovery::SessionInfo& source, QString* error) {
    const QDir root(QString::fromStdString(source.directory));
    QFile file(root.filePath("workspace.json"));
    QJsonArray entries;
    QString generation, selected;
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
        const auto manifest = QJsonDocument::fromJson(file.readAll()).object();
        if (manifest.value("format").toString() != "vlt-workspace-recovery" || manifest.value("version").toInt() != 1) {
            if (error) *error = tr("Unsupported workspace recovery format."); return false;
        }
        entries = manifest.value("documents").toArray();
        generation = manifest.value("generation").toString(); selected = manifest.value("selected").toString();
        if (entries.isEmpty() || (!generation.startsWith("generation-") && !generation.startsWith("g-")) || generation.contains('/') || generation.contains('\\')) {
            if (error) *error = tr("Invalid workspace recovery manifest."); return false;
        }
    } else entries.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"path", QString::fromStdString(source.projectPath)}});
    std::vector<std::unique_ptr<ProjectSession>> restored;
    QSet<QString> identities;
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        const auto id = entry.value("id").toString();
        if (QUuid(id).isNull() || identities.contains(id)) { if (error) *error = tr("Invalid project identity in recovery."); return false; }
        identities.insert(id);
        auto session = std::make_unique<ProjectSession>();
        session->controller.useWorkspacePluginManager(m_projects.plugins);
        session->controller.initializeDocumentOnly();
        auto record = source;
        record.projectPath = entry.value("path").toString().toStdString();
        if (!generation.isEmpty()) {
            const auto directory = entry.value("directory").toString(id);
            if (directory.isEmpty() || directory == "." || directory == ".." || directory.contains('/') || directory.contains('\\')) {
                if (error) *error = tr("Invalid project identity in recovery."); return false;
            }
            record.directory = root.filePath("Documents/"+generation+"/"+directory).toStdString();
        }
        if (!ui::recovery::applySession(record, session->controller, error)) return false;
        if (!m_projects.find(id)) session->id = id;
        if (selected == id) selected = session->id;
        session->path = QString::fromStdString(record.projectPath); session->dirty = true;
        const auto view = entry.value("view").toObject();
        session->view = view.toVariantMap();
        session->controller.seekSeconds(view.value("cursor").toDouble());
        session->selectedTrack = view.value("selectedTrack").toString();
        for (const auto& track : view.value("selectedTracks").toArray()) session->selectedTracks.append(track.toString());
        for (const auto& clip : view.value("selectedClips").toArray())
            session->selectedClips.append({clip.toObject().value("track").toString(), clip.toObject().value("clip").toString()});
        configureProjectSession(*session);
        restored.push_back(std::move(session));
    }
    if (m_midiInput) m_midiInput->setProjectController(nullptr);
    if (const auto stopped = m_projects.audio.activate(nullptr); !stopped) {
        if (m_midiInput) m_midiInput->setProjectController(m_projects.audio.active());
        if (error) *error = QString::fromStdString(stopped.message()); return false;
    }
    QList<ProjectId> previous; for (const auto& session : m_projects.documents) previous.append(session->id);
    const auto first = restored.front()->id;
    // A recovery set may predate the one-project preference. Never hide or
    // discard its other unsaved documents: restoring the set enables its tabs.
    if (restored.size() > 1 && !m_projectTabsEnabled) {
        m_projectTabsEnabled = true;
        QSettings().setValue("projects/tabsEnabled", true);
        statusBar()->showMessage(tr("Project tabs were enabled to restore all recovered projects."), 8000);
    }
    for (auto& session : restored) m_projects.documents.push_back(std::move(session));
    selectProjectTab(m_projects.find(selected) ? selected : first);
    for (const auto& id : previous) if (auto* session = m_projects.find(id)) session->dirty=false;
    closeProjectTabs(previous);
    refreshProjectTabs();
    return true;
}
