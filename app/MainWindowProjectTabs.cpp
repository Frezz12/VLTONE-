#include "MainWindow.hpp"
#include "McpService.hpp"
#include "ScriptService.hpp"
#include "TimelineWidget.hpp"
#include "TrackListWidget.hpp"
#include "TrackOverviewPanel.hpp"
#include "TransportBar.hpp"
#include "MixerWidget.hpp"
#include "InspectorWidget.hpp"
#include "ContextPanel.hpp"
#include "MasterPanel.hpp"
#include "FileBrowserPanel.hpp"
#include "AiChatPanel.hpp"
#include "NotebookWindow.hpp"
#include "PluginEditorWindow.hpp"
#include "SampleEditorWindow.hpp"
#include "AutomationEditorWindow.hpp"
#include "PianoRollWindow.hpp"
#include "PatternWindow.hpp"
#include "RackWidget.hpp"
#include "NoteContextPanel.hpp"
#include "WarpEditorWidget.hpp"
#include "InternalEditorFrame.hpp"
#include "MidiInputManager.hpp"
#include "TypingKeyboard.hpp"
#include "SettingsWindow.hpp"
#include "PerformanceWindow.hpp"
#include "PluginManagerWindow.hpp"
#include "ProjectDialogs.hpp"
#include "BackgroundPreparation.hpp"
#include "ProjectLoadDialog.hpp"
#include "RackDrag.hpp"
#include "ShortcutManager.hpp"
#include "Theme.hpp"
#include "Icons.hpp"
#include <QAction>
#include <QApplication>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QFileInfo>
#include <QLabel>
#include <QSettings>
#include <QCryptographicHash>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QString normalizedProjectPath(const QString& path) {
    if (path.isEmpty()) return {};
    QFileInfo info(path);
    if (info.isFile() && info.fileName().compare("Project.vlt", Qt::CaseInsensitive) == 0)
        info.setFile(info.absolutePath());
    auto result = info.canonicalFilePath();
    if (result.isEmpty()) result = info.absoluteFilePath();
#ifdef Q_OS_WIN
    result = result.toCaseFolded();
#endif
    return result;
}

class ProjectTabBar final : public QTabBar {
public:
    explicit ProjectTabBar(QWidget* parent) : QTabBar(parent) {
        setAcceptDrops(true);
        m_hover.setSingleShot(true); m_hover.setInterval(500);
        connect(&m_hover, &QTimer::timeout, this, [this] {
            if (m_hovered >= 0) setCurrentIndex(m_hovered);
        });
    }
protected:
    void dragEnterEvent(QDragEnterEvent* event) override { event->acceptProposedAction(); }
    void dragMoveEvent(QDragMoveEvent* event) override {
        const int index = tabAt(event->position().toPoint());
        if (index != m_hovered) { m_hovered = index; m_hover.start(); }
        event->acceptProposedAction();
    }
    void dragLeaveEvent(QDragLeaveEvent*) override { m_hover.stop(); m_hovered = -1; }
    void dropEvent(QDropEvent* event) override { m_hover.stop(); m_hovered = -1; event->ignore(); }
private:
    QTimer m_hover;
    int m_hovered = -1;
};
}

void MainWindow::buildProjectTabs(QVBoxLayout* layout) {
    m_projectTabsEnabled = QSettings().value("projects/tabsEnabled", true).toBool();
    auto* row = new QWidget(m_workspace);
    m_projectTabStrip = row;
    row->setObjectName("ProjectTabStrip");
    row->setProperty("dawPreserveEditorFocus", true);
    auto* bar = new QHBoxLayout(row);
    bar->setContentsMargins(8, 4, 8, 4); bar->setSpacing(6);
    m_projectTabs = new ProjectTabBar(row);
    m_projectTabs->setObjectName("ProjectTabs");
    m_projectTabs->setProperty("projectTabDropTarget", true);
    m_projectTabs->setAccessibleName(tr("Projects"));
    m_projectTabs->setMovable(true); m_projectTabs->setTabsClosable(true);
    m_projectTabs->setExpanding(false); m_projectTabs->setUsesScrollButtons(true);
    m_projectTabs->setElideMode(Qt::ElideRight);
    m_projectTabs->setDocumentMode(true);
    m_projectTabs->setDrawBase(false);
    m_projectTabs->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_projectTabs, &QWidget::customContextMenuRequested, this, [this](QPoint position) {
        const int index = m_projectTabs->tabAt(position);
        if (index < 0 || m_projectFileJob || m_projects.audio.busy() || m_switchingProject || m_closingProjects) return;
        auto* menu = createProjectTabMenu(m_projectTabs->tabData(index).toString());
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(m_projectTabs->mapToGlobal(position));
    });
    m_projectTabs->setIconSize(QSize(16, 16));
    bar->addWidget(m_projectTabs, 1);
    auto* plus = new QToolButton(row);
    plus->setToolTip(tr("New project")); plus->setAccessibleName(plus->toolTip());
    plus->setFixedSize(28, 28); plus->setIconSize(QSize(16, 16));
    bar->addWidget(plus);
    connect(plus, &QToolButton::clicked, this, [this] { addProjectTab(); });
    auto* overflow = new QToolButton(row);
    overflow->setObjectName("ProjectTabOverflow");
    overflow->setToolTip(tr("All projects")); overflow->setAccessibleName(overflow->toolTip());
    overflow->setFixedSize(28, 28); overflow->setIconSize(QSize(16, 16));
    overflow->setStyleSheet("#ProjectTabOverflow::menu-indicator { image: none; width: 0; }");
    const auto refreshNavigationIcons = [plus, overflow] {
        plus->setIcon(icons::icon(icons::Glyph::Plus, th().textPrimary, 16));
        overflow->setIcon(icons::icon(icons::Glyph::Chevron, th().textPrimary, 16));
    };
    refreshNavigationIcons();
    connect(&ThemeManager::instance(), &ThemeManager::changed, row, refreshNavigationIcons);
    auto* list = new QMenu(overflow); overflow->setMenu(list);
    overflow->setPopupMode(QToolButton::InstantPopup); bar->addWidget(overflow);
    connect(list, &QMenu::aboutToShow, this, [this, list] {
        list->clear();
        for (int i = 0; i < m_projectTabs->count(); ++i) {
            const auto id = m_projectTabs->tabData(i).toString();
            auto* action = list->addAction(m_projectTabs->tabText(i));
            action->setCheckable(true); action->setChecked(id == m_projects.selected);
            connect(action, &QAction::triggered, this, [this, id] { selectProjectTab(id); });
        }
    });
    m_projectPower = new QToolButton(row);
    m_projectPower->setCheckable(true); m_projectPower->setObjectName("ProjectEnginePower");
    m_projectPower->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_projectPower->setFixedSize(30, 30); m_projectPower->setIconSize(QSize(18, 18));
    bar->addWidget(m_projectPower);
    connect(m_projectPower, &QToolButton::clicked, this, [this] { toggleProjectAudio(); });
    m_audioOwnerButton = new QToolButton(row); m_audioOwnerButton->setObjectName("AudioProjectOwner");
    m_audioOwnerButton->setMaximumWidth(240);
    m_audioOwnerButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_audioOwnerButton->setIconSize(QSize(16, 16));
    bar->addWidget(m_audioOwnerButton);
    connect(m_audioOwnerButton, &QToolButton::clicked, this, [this] {
        if (auto* owner = m_projects.audible()) selectProjectTab(owner->id);
    });
    connect(m_projectTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (!m_switchingProject && index >= 0) selectProjectTab(m_projectTabs->tabData(index).toString());
    });
    connect(m_projectTabs, &QTabBar::tabCloseRequested, this, [this](int index) {
        closeProjectTabs({m_projectTabs->tabData(index).toString()});
    });
    connect(m_projectTabs, &QTabBar::tabMoved, this, [this](int from, int to) {
        auto session = std::move(m_projects.documents[from]);
        m_projects.documents.erase(m_projects.documents.begin() + from);
        m_projects.documents.insert(m_projects.documents.begin() + to, std::move(session));
        m_journalStale = true;
    });
    auto* menu = menuBar()->addMenu(tr("Projects"));
    m_projectTabsModeAction = menu->addAction(tr("Use project tabs"));
    m_projectTabsModeAction->setCheckable(true);
    connect(m_projectTabsModeAction, &QAction::triggered, this, [this](bool enabled) { setProjectTabsEnabled(enabled); });
    m_projectEngineAction = menu->addAction(tr("Enable engine"));
    connect(m_projectEngineAction, &QAction::triggered, this, [this] { toggleProjectAudio(); });
    menu->addSeparator();
    const auto command = [&](const QString& id, const QString& title, const QKeySequence& keys, auto callback) {
        auto* action = menu->addAction(title);
        m_shortcuts->registerCommand(id, title, tr("Projects"), keys, action);
        connect(action, &QAction::triggered, this, callback);
    };
    command("project.closeTab", tr("Close tab"), QKeySequence::Close, [this] { closeProjectTabs({m_projects.selected}); });
    command("project.closeOthers", tr("Close other tabs"), {}, [this] {
        QList<ProjectId> ids;
        for (const auto& s : m_projects.documents) if (s->id != m_projects.selected) ids.append(s->id);
        closeProjectTabs(ids);
    });
    command("project.closeAll", tr("Close all tabs"), {}, [this] {
        QList<ProjectId> ids; for (const auto& s : m_projects.documents) ids.append(s->id);
        closeProjectTabs(ids);
    });
    command("project.saveAll", tr("Save all"), {}, [this] { saveAllProjectTabs(); });
    command("project.nextTab", tr("Next project"), QKeySequence(Qt::CTRL | Qt::Key_Tab), [this] {
        m_projectTabs->setCurrentIndex((m_projectTabs->currentIndex() + 1) % m_projectTabs->count());
    });
    command("project.previousTab", tr("Previous project"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), [this] {
        m_projectTabs->setCurrentIndex((m_projectTabs->currentIndex() + m_projectTabs->count() - 1) % m_projectTabs->count());
    });
    const auto theme = [this, row] {
        const auto& t = th();
        row->setStyleSheet(QString(
            "#ProjectTabStrip { background: %1; }"
            "#ProjectTabs::tab { background: transparent; color: %2; border: 1px solid transparent;"
            " border-radius: 6px; min-width: 104px; max-width: 224px; padding: 4px 10px; margin-right: 3px; }"
            "#ProjectTabs::tab:hover { background: %3; color: %4; }"
            "#ProjectTabs::tab:selected { background: %3; color: %4; border-color: %5; }"
            "#ProjectTabStrip QToolButton { border: 1px solid transparent; border-radius: 6px; padding: 3px; }"
            "#ProjectTabStrip QToolButton:hover { background: %3; border-color: %5; }"
            "#ProjectTabStrip QToolButton:pressed { background: %5; }"
            "#ProjectEnginePower:checked { background: %3; border-color: %6; }"
            "#ProjectTabStrip QToolButton:focus { border-color: %6; }"
            "#ProjectTabStrip #ProjectTabClose { padding: 0; margin: 0; }"
            "#ProjectTabs #ProjectTabEngineState { padding: 0; margin: 0; border: none; background: transparent; }"
            "#ProjectTabOverflow::menu-indicator { image: none; width: 0; }")
            .arg(t.headerBackground.name(), t.textSecondary.name(), t.surfaceElevated.name(),
                 t.textPrimary.name(), t.separator().name(), t.accent.name()));
        refreshProjectTabs();
    };
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, theme); theme();
    layout->addWidget(row);
    m_projectTabsReady = true;
    refreshProjectTabs();
}

void MainWindow::refreshProjectTabs() {
    if (!m_projectTabs) return;
    m_projectTabStrip->setVisible(m_projectTabsEnabled);
    if (m_projectTabsModeAction) {
        const QSignalBlocker mode(m_projectTabsModeAction);
        m_projectTabsModeAction->setChecked(m_projectTabsEnabled);
    }
    for (const auto* id : {"project.closeOthers", "project.nextTab", "project.previousTab"})
        if (const auto* command = m_shortcuts->command(id); command && command->action)
            command->action->setEnabled(m_projectTabsEnabled && m_projects.documents.size() > 1);
    auto& selected = m_projects.current();
    selected.path = m_projectPath; selected.dirty = m_dirty; selected.revision = m_projectRevision;
    const QSignalBlocker block(m_projectTabs);
    while (m_projectTabs->count() > int(m_projects.documents.size())) m_projectTabs->removeTab(m_projectTabs->count()-1);
    while (m_projectTabs->count() < int(m_projects.documents.size())) m_projectTabs->addTab({});
    auto* owner = m_projects.audible();
    const auto onIcon = icons::icon(icons::Glyph::Waveform, th().accent, 16);
    const auto offIcon = icons::icon(icons::Glyph::Power, th().textSecondary, 16);
    for (int i = 0; i < int(m_projects.documents.size()); ++i) {
        const auto& session = *m_projects.documents[i];
        QString name = QString::fromStdString(session.controller.projectName());
        if (name.isEmpty() || name == "Untitled") name = tr("Untitled");
        const QString title = name + (session.dirty ? QString::fromUtf8(" •") : QString());
        m_projectTabs->setTabData(i, session.id); m_projectTabs->setTabText(i, title);
        // Reserve separate slots for the status and close glyphs: their
        // measured bounds must survive title elision and Windows DPI changes.
        auto* indicator = qobject_cast<QLabel*>(m_projectTabs->tabButton(i, QTabBar::LeftSide));
        if (!indicator) {
            indicator = new QLabel(m_projectTabs);
            indicator->setObjectName("ProjectTabEngineState");
            indicator->setFixedSize(20, 24);
            indicator->setAlignment(Qt::AlignCenter);
            indicator->setAttribute(Qt::WA_TransparentForMouseEvents);
            m_projectTabs->setTabButton(i, QTabBar::LeftSide, indicator);
        }
        m_projectTabs->setTabIcon(i, {});
        indicator->setPixmap((owner == &session ? onIcon : offIcon).pixmap(QSize(16, 16), devicePixelRatioF()));
        auto* close = qobject_cast<QToolButton*>(m_projectTabs->tabButton(i, QTabBar::RightSide));
        if (!close || close->objectName() != "ProjectTabClose") {
            auto* previous = m_projectTabs->tabButton(i, QTabBar::RightSide);
            close = new QToolButton(m_projectTabs);
            close->setObjectName("ProjectTabClose");
            close->setFixedSize(24, 24); close->setIconSize(QSize(16, 16));
            close->setAutoRaise(true);
            m_projectTabs->setTabButton(i, QTabBar::RightSide, close);
            if (previous) previous->deleteLater();
            connect(close, &QToolButton::clicked, this, [this, close] {
                closeProjectTabs({close->property("projectId").toString()});
            });
        }
        close->setProperty("projectId", session.id);
        close->setIcon(icons::icon(icons::Glyph::Close, th().textSecondary, 16));
        close->setToolTip(tr("Close tab"));
        close->setAccessibleName(tr("Close tab") + ": " + name);
        const QString description = name + " — " + (owner == &session ? tr("Engine on") : tr("Engine off"));
        m_projectTabs->setTabToolTip(i, description + (session.path.isEmpty() ? QString() : "\n" + session.path));
        m_projectTabs->setAccessibleTabName(i, description + (session.dirty ? ", " + tr("Unsaved changes") : ""));
        if (session.id == m_projects.selected) m_projectTabs->setCurrentIndex(i);
    }
    m_projectPower->setChecked(owner == &selected);
    const bool switchingEngine = m_projects.audio.busy() || m_projectPower->property("engineSwitching").toBool();
    const auto powerAction = switchingEngine ? tr("Switching engine…") : owner == &selected ? tr("Disable engine") : tr("Enable engine");
    if (m_projectEngineAction) {
        m_projectEngineAction->setText(powerAction);
        m_projectEngineAction->setIcon(icons::icon(icons::Glyph::Power, th().textPrimary, 16));
        m_projectEngineAction->setEnabled(!switchingEngine);
    }
    m_projectPower->setText({});
    m_projectPower->setIcon(icons::icon(switchingEngine ? icons::Glyph::Clock : icons::Glyph::Power,
        owner == &selected ? th().accent : th().textPrimary, 18));
    m_projectPower->setToolTip(powerAction);
    m_projectPower->setAccessibleName(powerAction);
    m_projectPower->setEnabled(!switchingEngine);
    const auto ownerTitle = owner ? tr("Engine: %1").arg(QString::fromStdString(owner->controller.projectName())) : tr("Engine off");
    m_audioOwnerButton->setText(m_audioOwnerButton->fontMetrics().elidedText(ownerTitle, Qt::ElideRight, 220));
    m_audioOwnerButton->setAccessibleName(ownerTitle); m_audioOwnerButton->setToolTip(ownerTitle);
    m_audioOwnerButton->setIcon(icons::icon(icons::Glyph::Waveform, th().accent, 16));
    m_audioOwnerButton->setVisible(owner && owner != &selected);
}

bool MainWindow::setProjectTabsEnabled(bool enabled) {
    if (m_projectFileJob || m_closingProjects || m_switchingProject || m_projects.audio.busy()) {
        refreshProjectTabs(); return false;
    }
    if (!enabled) {
        QList<ProjectId> others;
        for (const auto& session : m_projects.documents)
            if (session->id != m_projects.selected) others.append(session->id);
        if (!others.isEmpty() && !closeProjectTabs(others)) { refreshProjectTabs(); return false; }
    }
    m_projectTabsEnabled = enabled;
    QSettings().setValue("projects/tabsEnabled", enabled);
    if (m_settingsWindow) m_settingsWindow->refreshProjectTabsPreference();
    refreshProjectTabs();
    return true;
}

QMenu* MainWindow::createProjectTabMenu(const ProjectId& id) {
    auto* menu = new QMenu(m_projectTabs);
    menu->setObjectName("ProjectTabContextMenu");
    const auto add = [&](const char* name, const QString& title, auto callback) {
        auto* action = menu->addAction(title);
        action->setObjectName(name);
        connect(action, &QAction::triggered, this, [this, id, callback] {
            if (!m_projects.find(id) || m_projectFileJob || m_closingProjects || m_switchingProject || m_projects.audio.busy()) return;
            callback();
        });
        return action;
    };
    add("project.tabSave", tr("Save project"), [this, id] { if (selectProjectTab(id)) onSaveProject(); });
    add("project.tabSaveAs", tr("Save project as…"), [this, id] { if (selectProjectTab(id)) onSaveProjectAs(); });
    menu->addSeparator();
    const auto* session = m_projects.find(id);
    add("project.tabEngine", session && m_projects.audio.active() == &session->controller ? tr("Disable engine") : tr("Enable engine"),
        [this, id] {
            const auto selected = m_projects.selected;
            if (selectProjectTab(id)) toggleProjectAudio();
            selectProjectTab(selected);
        });
    menu->addSeparator();
    add("project.tabClose", tr("Close tab"), [this, id] { closeProjectTabs({id}); });
    add("project.tabCloseOthers", tr("Close other tabs"), [this, id] {
        QList<ProjectId> others;
        for (const auto& session : m_projects.documents) if (session->id != id) others.append(session->id);
        closeProjectTabs(others);
    })->setEnabled(m_projects.documents.size() > 1);
    add("project.tabCloseAll", tr("Close all tabs"), [this] {
        QList<ProjectId> ids;
        for (const auto& session : m_projects.documents) ids.append(session->id);
        closeProjectTabs(ids);
    });
    return menu;
}

void MainWindow::tagProjectEditor(QWidget* editor) {
    editor->setProperty("vlt.projectId", m_projects.selected);
}

void MainWindow::captureProjectView() {
    auto& session = m_projects.current();
    session.path = m_projectPath; session.dirty = m_dirty; session.revision = m_projectRevision;
    session.selectedTrack = m_selectedTrackId; session.selectedTracks = m_trackList->selectedTrackIds();
    if (!m_switchingProject) session.midiTarget = liveInputTarget();
    session.selectedClips = m_selection.clipsInSelectionOrder();
    session.recordPins = m_recordPins; session.recordEngaged = m_recordEngaged;
    session.view["zoom"] = m_timeline->pixelsPerSecondForTest();
    session.view["scrollX"] = m_timeline->horizontalScrollForTest();
    session.view["scrollY"] = m_timeline->verticalScroll();
    session.view["regionTracks"] = m_timeline->regionTrackIds();
    session.view["regionStart"] = m_timeline->regionStartSeconds();
    session.view["regionEnd"] = m_timeline->regionEndSeconds();
    session.view["mixer"] = m_bottomPanel->isVisible();
    session.view["browser"] = m_browser->isVisible();
    session.view["master"] = m_masterPanel && m_masterPanel->isVisible();
    session.view["inspector"] = m_inspector->isVisible();
    session.view["context"] = m_contextPanel->isVisible();
    session.view["overview"] = m_trackOverview && m_trackOverview->isVisible();
    session.view["notebook"] = m_notebookContainer->isVisible();
    session.view["playRangeStart"] = m_playRangeStart; session.view["playRangeEnd"] = m_playRangeEnd;
    session.view["browserSampleTarget"] = m_browserSampleTargetTrackId;
    session.view["browserPreserveSampler"] = m_browserSampleTargetPreservesSampler;
    session.pluginEditors.clear(); session.sampleEditors.clear(); session.automationEditors.clear();
    for (auto it=m_pluginEditors.begin(); it!=m_pluginEditors.end(); ++it) session.pluginEditors[it.key()]=it.value();
    for (auto it=m_sampleEditors.begin(); it!=m_sampleEditors.end(); ++it) session.sampleEditors[it.key()]=it.value();
    for (auto it=m_automationEditors.begin(); it!=m_automationEditors.end(); ++it) session.automationEditors[it.key()]=it.value();
    session.piano = m_pianoRoll; session.pianoFrame = m_pianoRollFrame; session.warp = m_warpEditor;
    session.noteContext = m_noteContextPanel;
    session.notebook = m_notebookWindow;
    session.view["rackMode"] = m_mixer->rackMode();
    if (m_patternWindow) {
        session.view["patternId"] = m_patternWindow->patternId();
        session.view["patternClipId"] = m_patternWindow->patternClipId();
        session.view["patternSources"] = m_patternWindow->selectedSourceTrackIds();
        session.view["patternPrimary"] = m_patternWindow->selectedSourceTrackId();
    }
    session.visibleEditors.clear();
    session.activeEditor = nullptr;
    for (auto it=m_internalEditorFrames.begin(); it!=m_internalEditorFrames.end(); ++it) {
        if (it.key()->property("vlt.projectId").toString() == session.id && it.value()->isVisible()) {
            session.visibleEditors.append(it.value());
            if (it.value()->isEditorActive()) session.activeEditor = it.value();
        }
    }
}

bool MainWindow::selectProjectTab(const ProjectId& id) {
    auto* next = m_projects.find(id);
    if (!next || m_projectFileJob || m_switchingProject) { refreshProjectTabs(); return false; }
    if (id == m_projects.selected) return true;
    m_projects.current().midiTarget = liveInputTarget();
    const QScopedValueRollback<bool> switching(m_switchingProject, true);
    // Complete continuous edits against the old document before publishing the
    // new identity. QObject children still point to their owning controller.
    if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
    m_timeline->finishProjectEdits();
    m_trackList->finishProjectEdits();
    if (m_pianoRoll) m_pianoRoll->finishPendingNoteEdit();
    if (m_warpEditor) m_warpEditor->finishPendingEdit();
    captureProjectView();
    // A native drag can defer destruction of the outgoing rack. Its callbacks
    // must no longer resolve commands through the newly selected document.
    if (m_patternWindow) disconnect(m_patternWindow, nullptr, this, nullptr);
    disconnect(m_mixer->rack(), nullptr, this, nullptr);
    m_timeline->setProjectController(&next->controller);
    m_mixer->setProjectController(&next->controller);
    m_patternWindow = nullptr;
    m_inspector->setProjectController(&next->controller);
    m_contextPanel->setProjectController(&next->controller);
    if (m_masterPanel) m_masterPanel->setProjectController(&next->controller);
    auto& old = m_projects.current();
    old.dirty = m_dirty; old.revision = m_projectRevision;
    for (auto& editor : old.visibleEditors) if (editor) editor->hide();
    if (m_noteContextPanel) m_noteContextPanel->hide();
    if (m_notebookDetached) setNotebookDetached(false);
    if (m_notebookWindow) m_notebookWindow->hide();
    delete m_settingsWindow; m_settingsWindow = nullptr;
    delete m_performanceWindow; m_performanceWindow = nullptr;
    m_pluginEditors.clear(); m_sampleEditors.clear(); m_automationEditors.clear(); m_liveInputEditor = nullptr;
    m_projects.selected = id;
    m_processingProjectGeneration = ~quint64(0);
    m_pendingUiChange = {};
    m_projectPath = next->path; m_dirty = next->dirty; m_projectRevision = next->revision;
    m_selectedTrackId = next->selectedTrack; m_recordPins = next->recordPins; m_recordEngaged = next->recordEngaged;
    m_browserSampleTargetTrackId = next->view.value("browserSampleTarget").toString();
    m_browserSampleTargetPreservesSampler = next->view.value("browserPreserveSampler", false).toBool();
    m_selection.clear();
    m_trackList->setProjectController(&next->controller);
    if (m_trackOverview) m_trackOverview->setProjectController(&next->controller);
    m_browser->setProjectController(&next->controller);
    if (m_pluginManagerWindow) m_pluginManagerWindow->setProjectController(&next->controller);
    m_transport->setProjectController(&next->controller); m_transport->setRecordEngaged(m_recordEngaged);
    m_typingKeyboard->setProjectController(&next->controller);
    m_aiPanel->setProjectController(&next->controller);
    for (auto it=next->pluginEditors.begin(); it!=next->pluginEditors.end(); ++it) if (it.value()) m_pluginEditors[it.key()]=qobject_cast<PluginEditorWindow*>(it.value());
    for (auto it=next->sampleEditors.begin(); it!=next->sampleEditors.end(); ++it) if (it.value()) m_sampleEditors[it.key()]=qobject_cast<SampleEditorWindow*>(it.value());
    for (auto it=next->automationEditors.begin(); it!=next->automationEditors.end(); ++it) if (it.value()) m_automationEditors[it.key()]=qobject_cast<AutomationEditorWindow*>(it.value());
    m_pianoRoll = qobject_cast<PianoRollWindow*>(next->piano.data());
    if (m_pianoRoll) m_pianoRoll->setLivePitches(livePitchesForTrack(m_pianoRoll->trackId()));
    m_pianoRollFrame = qobject_cast<InternalEditorFrame*>(next->pianoFrame.data());
    m_noteContextPanel = qobject_cast<NoteContextPanel*>(next->noteContext.data());
    m_warpEditor = qobject_cast<WarpEditorWidget*>(next->warp.data());
    m_notebookWindow = qobject_cast<NotebookWindow*>(next->notebook.data());
    connectPatternRack();
    if (m_notebookWindow && m_notebookContainer->isVisible()) m_notebookWindow->show();
    m_timeline->setSelectedTrack(m_selectedTrackId);
    m_timeline->setSelectedTracks(next->selectedTracks);
    m_timeline->restoreProjectSelection(next->selectedClips);
    if (next->selectedClips.isEmpty()) m_selection.setTracks(next->selectedTracks);
    m_trackList->setSelectedTracks(next->selectedTracks, next->selectedTrack);
    syncViews();
    m_mixer->setRackMode(next->view.value("rackMode", false).toBool());
    const auto patternId = next->view.value("patternId").toString();
    if (const auto* pattern = next->controller.project().findTrack(patternId.toStdString());
        pattern && pattern->kind == daw::TrackKind::Pattern) {
        m_mixer->rack()->setPattern(patternId, next->view.value("patternClipId").toString());
        m_patternWindow->setSelectedSources(next->view.value("patternSources").toStringList(),
                                           next->view.value("patternPrimary").toString());
    }
    if (!next->view.isEmpty()) {
        setMixerVisible(next->view.value("mixer", true).toBool());
        setBrowserVisible(next->view.value("browser", true).toBool());
        setMasterVisible(next->view.value("master", true).toBool());
        setInspectorVisible(next->view.value("inspector", true).toBool());
        setContextPanelVisible(next->view.value("context", true).toBool());
        setTrackOverviewVisible(next->view.value("overview", false).toBool());
        setNotebookVisible(next->view.value("notebook", false).toBool(), false);
    }
    m_timeline->setHorizontalZoom(next->view.value("zoom", 80.).toDouble());
    m_timeline->setHorizontalScrollForTest(next->view.value("scrollX", 0.).toDouble());
    m_timeline->setVerticalScroll(next->view.value("scrollY", 0).toInt());
    m_timeline->restoreProjectRegion(next->view.value("regionStart").toDouble(),
                                    next->view.value("regionEnd").toDouble(),
                                    next->view.value("regionTracks").toStringList());
    updateLocalProcessingActions();
    for (auto& editor : next->visibleEditors) if (editor) editor->show();
    if (auto* frame = qobject_cast<InternalEditorFrame*>(next->activeEditor.data()); frame && frame->isVisible())
        frame->activateEditor();
    syncPianoRollContextPanel();
    m_playRangeStart = next->view.value("playRangeStart", -1.).toDouble();
    m_playRangeEnd = next->view.value("playRangeEnd", -1.).toDouble(); m_hadRecordingRows = false;
    m_recoveryAllTracksDirty = true; m_recoveryDirtyTracks.clear(); m_recoveryPluginStateFiles.clear();
    m_journalStale = true;
    updateWindowTitle(); syncPlayheadTimer(); refreshProjectTabs();
    return true;
}

void MainWindow::configureProjectSession(ProjectSession& session) {
    session.controller.setDefaultTrackColor(uint32_t(th().accent.rgb()) & 0x00FFFFFFu);
    if (&session != &m_projects.current()) {
        session.controller.setPlaybackMode(projectController().playbackMode());
        session.controller.setRecordingPrefs(projectController().recordingPrefs());
    }
    const auto id = session.id;
    // Track UUIDs scope these user-local assignments to project instruments;
    // they never travel in shared snapshots or plugin state.
    const auto projectPath = [this, id] {
        if (id == m_projects.selected) return m_projectPath;
        const auto* owner = m_projects.find(id);
        return owner ? owner->path : QString();
    };
    const auto midiBindingKey=[this, id, projectPath](const std::string& track) {
        const auto path = projectPath();
        QString scope=path.isEmpty()?QStringLiteral("unsaved"):QFileInfo(path).absoluteFilePath();
#ifdef DAW_ENABLE_COLLABORATION
        if(id == m_projects.selected && !m_cloudProjectId.isEmpty())scope=m_cloudProjectId;
#endif
        return QStringLiteral("midiLearn/")+QString::fromLatin1(QCryptographicHash::hash(scope.toUtf8(),QCryptographicHash::Sha256).toHex())+"/"+QString::fromStdString(track);
    };
    session.controller.loadLocalMidiBindings = [midiBindingKey](const std::string& track) {
        std::vector<daw::EngineController::MidiLearnBinding> bindings;
        const auto rows = QSettings().value(midiBindingKey(track),QSettings().value("midiLearn/unsaved/"+QString::fromStdString(track))).toList();
        for (const auto& row : rows) {
            const auto b = row.toMap();
            bindings.push_back({track, b.value("instrument").toString().toStdString(),
                b.value("parameter").toString().toStdString(), b.value("name").toString().toStdString(),
                b.value("channel").toInt(), b.value("cc").toInt()});
        }
        return bindings;
    };
    session.controller.saveLocalMidiBindings = [projectPath,midiBindingKey](const std::string& track, const auto& bindings) {
        QVariantList rows;
        for (const auto& b : bindings) rows.push_back(QVariantMap{
            {"instrument", QString::fromStdString(b.instrumentUid)}, {"parameter", QString::fromStdString(b.parameterId)},
            {"name", QString::fromStdString(b.name)}, {"channel", b.channel}, {"cc", b.cc}});
        QSettings().setValue(midiBindingKey(track), rows);
        if(projectPath().isEmpty())QSettings().setValue("midiLearn/unsaved/"+QString::fromStdString(track),rows);
    };
    session.controller.setPluginRetiringCallback([this, id](const std::string& channel, const std::string& slot) {
        retireProjectEditor(id, QString::fromStdString(channel), QString::fromStdString(slot));
    });
}

void MainWindow::retireProjectEditor(const ProjectId& id, const QString& channel, const QString& slot) {
    if (id == m_projects.selected && !m_projects.audio.busy()) { retirePluginEditor(channel, slot); return; }
    auto* session = m_projects.find(id);
    if (!session) return;
    const auto key = channel + '/' + slot;
    auto* editor = id == m_projects.selected ? m_pluginEditors.value(key) : qobject_cast<PluginEditorWindow*>(session->pluginEditors.value(key).data());
    if (editor) { editor->detachFromPlugin(); delete editor; }
    session->pluginEditors.remove(key);
}

bool MainWindow::addProjectTab(const QString& path, bool fromTemplate) {
    if (m_projectFileJob || m_projects.audio.busy() || m_switchingProject || m_closingProjects) return false;
    if (!path.isEmpty() && !fromTemplate) {
        captureProjectView();
        for (const auto& session : m_projects.documents)
            if (normalizedProjectPath(session->path) == normalizedProjectPath(path)) {
                const bool selected = selectProjectTab(session->id);
                if (selected && m_projects.audio.active() != &projectController()) toggleProjectAudio();
                return selected;
            }
    }
    auto candidate = std::make_unique<ProjectSession>();
    candidate->controller.useWorkspacePluginManager(m_projects.plugins);
    auto result = candidate->controller.initializeDocumentOnly(projectController().sampleRate(), projectController().bufferSizeFrames());
    std::unique_ptr<ui::ProjectLoadDialog> progress;
    if (result && path.isEmpty()) result = candidate->controller.newProject(true);
    else if (result) {
        progress = std::make_unique<ui::ProjectLoadDialog>(this, QFileInfo(path).completeBaseName());
        const QScopedValueRollback<bool> fileJob(m_projectFileJob, true);
        struct PreparedOpen { daw::EngineController::PreparedProject project; audio::Result result = audio::Result::ok(); };
        try {
            auto prepared = ui::prepareInBackground<PreparedOpen>(this, progress->labelText(),
                [path = path.toStdString(), rate = candidate->controller.sampleRate()](const auto& keepGoing) {
                    PreparedOpen prepared;
                    prepared.result = daw::EngineController::prepareProjectOpen(path, rate, prepared.project, keepGoing);
                    return prepared;
                }, true, progress.get());
            if (!prepared) return false;
            result = prepared->result;
            progress->stage(ui::ProjectLoadDialog::tr("Preparing project…"), false);
            if (result) result = candidate->controller.openPreparedProject(std::move(prepared->project), fromTemplate);
        } catch (const std::exception& error) {
            result = audio::Result::fail(audio::EngineError::Unknown, error.what());
        }
    }
    if (!result) { progress.reset(); QMessageBox::warning(this, tr("Open Project Failed"), QString::fromStdString(result.message())); return false; }
    candidate->path = fromTemplate ? QString() : path;
    configureProjectSession(*candidate);
    const auto id = candidate->id;
    const bool restartSingleAudio = !m_projectTabsEnabled && m_projects.audio.active();
    if (!m_projectTabsEnabled) {
        QList<ProjectId> previous;
        for (const auto& session : m_projects.documents) previous.append(session->id);
        // Candidate stays private until all save decisions succeed. A failed
        // read, cancelled save or failed recording finalization keeps the old document.
        if (progress) progress->hide();
        if (!closeProjectTabs(previous, std::move(candidate))) return false;
    } else m_projects.documents.push_back(std::move(candidate));
    if (!path.isEmpty() && !fromTemplate) ui::rememberRecentProject(path);
    refreshProjectTabs();
    const bool selected = selectProjectTab(id);
    if (selected && (!path.isEmpty() || restartSingleAudio)) toggleProjectAudio(progress.get());
    return selected;
}

bool MainWindow::openStartupPath(const QString& path) {
    // The initial launch document uses the already prepared startup engine.
    // Subsequent OS open requests create dormant tabs through the normal path.
    const QScopedValueRollback<bool> startup(m_projectTabsReady, false);
    const bool opened = openExternalPath(path);
    refreshProjectTabs();
    return opened;
}

bool MainWindow::requireProjectAudio() {
    if (m_projects.audio.active() == &projectController()) return true;
    QMessageBox prompt(QMessageBox::Information, tr("Engine off"),
        tr("Enable this project's engine to play, record or process audio."), QMessageBox::Cancel, this);
    auto* enable = prompt.addButton(tr("Enable engine"), QMessageBox::AcceptRole);
    {
        const QScopedValueRollback<bool> switching(m_switchingProject, true);
        prompt.exec();
    }
    if (prompt.clickedButton() == enable) toggleProjectAudio();
    return false; // Enabling never starts transport as a side effect.
}

void MainWindow::toggleProjectAudio(ui::ProjectLoadDialog* progress) {
    if (m_projectFileJob || m_projects.audio.busy() || m_switchingProject) return;
    const QScopedValueRollback<bool> switching(m_switchingProject, true);
    if (m_midiInput) QCoreApplication::sendPostedEvents(m_midiInput, QEvent::MetaCall);
    if (auto* focus = QApplication::focusWidget()) focus->clearFocus();
    m_timeline->finishProjectEdits();
    m_mixer->finishProjectEdits();
    m_trackList->finishProjectEdits();
    if (m_pianoRoll) m_pianoRoll->finishPendingNoteEdit();
    if (m_warpEditor) m_warpEditor->finishPendingEdit();
    delete m_settingsWindow; m_settingsWindow = nullptr;
    delete m_performanceWindow; m_performanceWindow = nullptr;
    auto* before = m_projects.audible();
    const bool wasRecording = before && before->controller.isRecording();
    std::unique_ptr<ui::ProjectLoadDialog> ownedProgress;
    if (!progress) {
        ownedProgress = std::make_unique<ui::ProjectLoadDialog>(this, QString::fromStdString(projectController().projectName()));
        progress = ownedProgress.get();
        progress->setWindowTitle(tr("Audio engine"));
    }
    progress->stage(before ? ui::ProjectLoadDialog::tr("Saving plugin states…") : ui::ProjectLoadDialog::tr("Preparing audio engine…"), false);
    progress->show();
    const ui::ProjectLoadDialog::Binding progressBinding(*progress, projectController());
    m_projectPower->setProperty("engineSwitching", true);
    refreshProjectTabs();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    m_midiInput->setProjectController(nullptr);
    const auto result = m_projects.audio.activate(before == &m_projects.current() ? nullptr : &projectController());
    m_midiInput->setProjectController(m_projects.audio.active());
    m_projectPower->setProperty("engineSwitching", false);
    progress->hide();
    if (wasRecording && before && !before->controller.isRecording()) {
        before->dirty = true; ++before->revision;
        if (before == &m_projects.current()) { m_dirty = true; ++m_projectRevision; syncViews(); }
    }
    if (!result) QMessageBox::warning(this, tr("Audio engine"), QString::fromStdString(result.message()));
    m_journalStale = true;
    refreshProjectTabs(); syncViews();
}

void MainWindow::serviceBackgroundProject() {
    auto* session = m_projects.audible();
    if (!session || session == &m_projects.current()) return;
    auto& controller = session->controller;
    const double loopEnd = session->view.value("playRangeEnd", -1.).toDouble();
    if (loopEnd >= 0. && controller.isPlaying() && controller.positionSeconds() >= loopEnd)
        controller.seekSeconds(session->view.value("playRangeStart", 0.).toDouble());
    if (controller.isRecording()) controller.pumpRecordingEnvelopes();
    const bool changed = controller.pumpPluginEvents();
    const auto automation = controller.followTouchedAutomation();
    if (changed || automation == daw::EngineController::AutomationFollowChange::Structure) {
        session->dirty = true; ++session->revision; m_journalStale = true;
    }
}

bool MainWindow::projectPathAvailable(const QString& path) const {
    for (const auto& session : m_projects.documents)
        if (session->id != m_projects.selected && !session->path.isEmpty() &&
            normalizedProjectPath(session->path) == normalizedProjectPath(path)) return false;
    return true;
}

void MainWindow::finishAudioRecordingForHealth() {
    if (&audioController() == &projectController()) { stopRecordingNow(); return; }
    auto* session = m_projects.audible();
    if (!session) return;
    session->controller.stopRecording();
    session->dirty = true; ++session->revision; m_journalStale = true;
    refreshProjectTabs();
}

bool MainWindow::saveAllProjectTabs() {
    if (m_projectFileJob || m_closingProjects || m_switchingProject || m_projects.audio.busy()) return false;
    captureProjectView();
    const auto selected = m_projects.selected;
    bool success = true;
    for (const auto& session : m_projects.documents) {
        if (!session->dirty) continue;
        if (!selectProjectTab(session->id)) { success = false; break; }
        onSaveProject();
        if (m_dirty) { success = false; break; }
    }
    selectProjectTab(selected);
    return success;
}

bool MainWindow::closeProjectTabs(const QList<ProjectId>& ids, std::unique_ptr<ProjectSession> replacement, bool interactive) {
    if (m_projectFileJob || m_closingProjects || m_switchingProject || m_projects.audio.busy() || ui::rack::nativeDragDepth || ids.isEmpty()) return false;
    for (const auto& id : ids) if (const auto* session = m_projects.find(id); session && session->controller.offlineRenderInProgress()) {
        statusBar()->showMessage(tr("Finish rendering before closing this project."), 5000);
        return false;
    }
    const QScopedValueRollback<bool> closing(m_closingProjects, true);
    captureProjectView();
    const auto selected = m_projects.selected;
    // Finalize a take before asking about saving it. Failure leaves every tab.
    if (auto* owner = m_projects.audible(); owner && ids.contains(owner->id) && owner->controller.isRecording()) {
        if (m_midiInput) QCoreApplication::sendPostedEvents(m_midiInput, QEvent::MetaCall);
        owner->controller.stopRecording();
        owner->dirty = true;
        if (owner == &m_projects.current()) m_dirty = true;
        if (!owner->controller.recordingWarning().empty()) {
            if (interactive) QMessageBox::warning(this, tr("Recording"), QString::fromStdString(owner->controller.recordingWarning()));
            return false;
        }
    }
    for (const auto& id : ids) {
        const auto* session = m_projects.find(id);
        if (!session || !session->dirty) continue;
        if (!interactive) return false;
        selectProjectTab(id);
        if (!maybeSaveChanges()) { selectProjectTab(selected); return false; }
    }
    if (auto* owner = m_projects.audible(); owner && ids.contains(owner->id)) {
        // These windows follow the audio owner, which can differ from the
        // selected tab. A clean background close need not switch the view.
        delete m_settingsWindow; m_settingsWindow = nullptr;
        delete m_performanceWindow; m_performanceWindow = nullptr;
        m_midiInput->setProjectController(nullptr);
        if (const auto stopped = m_projects.audio.activate(nullptr); !stopped) {
            m_midiInput->setProjectController(m_projects.audio.active());
            if (interactive) QMessageBox::warning(this, tr("Audio engine"), QString::fromStdString(stopped.message()));
            selectProjectTab(selected); return false;
        }
    }
    ProjectId keep;
    for (const auto& session : m_projects.documents) if (!ids.contains(session->id)) {
        if (keep.isEmpty() || session->id == selected) keep = session->id;
    }
    if (keep.isEmpty()) {
        if (replacement) {
            keep = replacement->id;
            m_projects.documents.push_back(std::move(replacement));
        } else {
            auto& blank = m_projects.append(); blank.controller.initializeDocumentOnly(); blank.controller.newProject(true);
            configureProjectSession(blank); keep = blank.id;
        }
    }
    selectProjectTab(keep);
    m_midiInput->setProjectController(m_projects.audio.active());
    // No editor, queued QObject callback or undo action may outlive its owner.
    for (const auto& id : ids) {
        if (m_mcp) m_mcp->projectClosing(id);
        if (m_scripts) m_scripts->closeProject(id);
        const auto widgets = findChildren<QWidget*>();
        QList<QPointer<QWidget>> owned;
        for (auto* widget : widgets) if (widget->property("vlt.projectId").toString() == id) owned.append(widget);
        for (auto& widget : owned) if (widget) delete widget;
    }
    std::erase_if(m_projects.documents, [&](const auto& session) { return ids.contains(session->id); });
    m_journalStale = true; refreshProjectTabs();
    return true;
}
