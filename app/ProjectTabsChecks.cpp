#include "MainWindow.hpp"
#include "TimelineWidget.hpp"
#include "TrackListWidget.hpp"
#include "ShortcutManager.hpp"
#include "InternalEditorFrame.hpp"
#include "PianoRollWindow.hpp"
#include "NoteContextPanel.hpp"
#include "PatternWindow.hpp"
#include "MixerWidget.hpp"
#include "ChannelStrip.hpp"
#include "ProjectLoadDialog.hpp"
#include "Internal/EqualizerInstance.hpp"
#include "MidiInputManager.hpp"
#include "SettingsWindow.hpp"
#include "PerformanceWindow.hpp"
#include <QCheckBox>
#include <QSettings>
#include <QApplication>
#include <QDir>
#include <QTabBar>
#include <QToolButton>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QEventLoop>
#include <QScopedValueRollback>
#include <QShortcutEvent>
#include <QAction>
#include "RecoverySupport.hpp"
#include <cmath>
#include <cstdio>

bool MainWindow::checkProjectTabsForTest(const QString& shots) {
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        std::fprintf(stderr, "%s tabs: %s\n", value ? "PASS" : "FAIL", message);
        failures += !value; return value;
    };
    resize(1280, 860); show(); QApplication::processEvents();
    if (!QSettings().contains("projects/tabsEnabled"))
        check(m_projectTabsEnabled && m_projectTabStrip->isVisible(), "project tabs are enabled and visible by default");
    check(setProjectTabsEnabled(true), "enable tabs for the workspace checks");
    const auto aId = m_projects.selected;
    auto* a = &projectController();
    const auto track = a->addTrack(daw::TrackKind::Midi, "A notes");
    const auto clip = a->addMidiClip(track, 2., 4.);
    a->setProjectName("A — длинное название проекта / Long project title");
    syncViews();
    m_timeline->selectClips({{QString::fromStdString(track), QString::fromStdString(clip)}});
    m_timeline->setHorizontalZoom(137.); a->seekSeconds(3.); a->play();
    const auto undoA = a->undoDepth();
    check(m_timeline->copySelection(), "capture independent clipboard");
    m_timeline->restoreProjectRegion(2.25, 3.75, {QString::fromStdString(track)});
    check(addProjectTab(), "create second document");
    const auto bId = m_projects.selected;
    auto* b = &projectController();
    check(a != b && m_projectTabs->count()==2 && b->documentOnly(), "separate stable controllers and inactive new tab");
    check(m_projects.audio.active()==a && a->isPlaying(), "view switch leaves audio owner playing");
    check(m_audioOwnerButton->isVisible(), "audio owner remains visible in other tab");
    check(m_timeline->pasteClipboard(), "paste clips into inactive destination");
    check(b->loadedPluginCount()==0 && a->undoDepth()==undoA, "paste leaves source undo and destination DSP untouched");
    const auto undoB = b->undoDepth();
    check(selectProjectTab(aId) && &projectController()==a, "return to original controller");
    check(m_selection.clips().size()==1 && m_selection.clips().front().clipId==QString::fromStdString(clip), "restore selected clip");
    check(std::abs(m_timeline->pixelsPerSecondForTest()-137.)<.001, "restore horizontal zoom");
    check(m_timeline->hasRegionSelection() && m_timeline->regionTrackIds()==QStringList{QString::fromStdString(track)} &&
          m_timeline->regionStartSeconds()==2.25 && m_timeline->regionEndSeconds()==3.75, "restore independent time-range selection");
    const auto* nextCommand = m_shortcuts->command("project.nextTab");
    const auto* previousCommand = m_shortcuts->command("project.previousTab");
    check(nextCommand && previousCommand && m_shortcuts->shortcut("project.nextTab")==QKeySequence(Qt::CTRL|Qt::Key_Tab) &&
          m_shortcuts->shortcut("project.previousTab")==QKeySequence(Qt::CTRL|Qt::SHIFT|Qt::Key_Tab), "tab navigation uses the shared shortcut manager");
    if (nextCommand && previousCommand) {
        QShortcutEvent nextKey(m_shortcuts->shortcut("project.nextTab"), 0);
        QApplication::sendEvent(nextCommand->action, &nextKey);
        check(m_projects.selected==bId && m_projects.audio.active()==a, "Ctrl+Tab changes view without stealing audio");
        QShortcutEvent previousKey(m_shortcuts->shortcut("project.previousTab"), 0);
        QApplication::sendEvent(previousCommand->action, &previousKey);
        check(m_projects.selected==aId, "Ctrl+Shift+Tab returns to the previous document");
    }
    openPianoRoll(QString::fromStdString(track), QString::fromStdString(clip));
    QApplication::processEvents();
    const QPointer<PianoRollWindow> pianoA = m_pianoRoll;
    const QPointer<InternalEditorFrame> frameA = m_pianoRollFrame;
    const QPointer<NoteContextPanel> notesA = m_noteContextPanel;
    const auto fallbackTrack = a->addTrack(daw::TrackKind::Midi, "Other MIDI target");
    selectTrackFromHeader(QString::fromStdString(fallbackTrack));
    frameA->activateEditor();
    m_midiInput->refreshTarget();
    m_midiInput->injectMessageForTest(QByteArray::fromHex("904064"));
    const auto tabPoint = m_projectTabs->tabRect(1).center();
    QMouseEvent tabPress(QEvent::MouseButtonPress, tabPoint, m_projectTabs->mapToGlobal(tabPoint), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent tabRelease(QEvent::MouseButtonRelease, tabPoint, m_projectTabs->mapToGlobal(tabPoint), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(m_projectTabs, &tabPress); QApplication::sendEvent(m_projectTabs, &tabRelease);
    check(m_projects.selected==bId && m_midiInput->heldCount()==1 && m_projects.find(aId)->midiTarget==track,
          "clicking a project tab preserves held MIDI and the piano's target track");
    check(!livePitchesForTrack(QString::fromStdString(track)).test(64) &&
          m_projects.find(aId)->livePitchCounts.value(QString::fromStdString(track))[64]==1,
          "held-note indication belongs to its document even when track IDs coincide");
    m_midiInput->injectMessageForTest(QByteArray::fromHex("804000"));
    check(frameA && !frameA->isVisible() && !m_pianoRoll && !m_noteContextPanel,
          "switch hides the source piano and its note panel");
    const auto bClip = m_selection.clips().front();
    openPianoRoll(bClip.trackId, bClip.clipId);
    const QPointer<PianoRollWindow> pianoB = m_pianoRoll;
    const QPointer<InternalEditorFrame> frameB = m_pianoRollFrame;
    const auto bRevision = m_projectRevision;
    emit pianoA->edited();
    check(m_projectRevision == bRevision && m_projects.find(aId)->dirty,
          "late piano edit notification belongs to the originating document");
    selectProjectTab(aId);
    check(m_pianoRoll == pianoA && m_noteContextPanel == notesA && frameA->isVisible() && !frameB->isVisible(),
          "each tab restores its own piano editor and note panel");
    check(!livePitchesForTrack(QString::fromStdString(track)).test(64),
          "notes released while a document is hidden stay released on return");
    if (m_pianoRollFrame) m_pianoRollFrame->activateEditor();
    m_timeline->selectClips({{QString::fromStdString(track), QString::fromStdString(clip)}});
    const auto* closeCommand = m_shortcuts->command("project.closeTab");
    if (closeCommand) {
        QShortcutEvent closeKey(m_shortcuts->shortcut("project.closeTab"), 0);
        QApplication::sendEvent(closeCommand->action, &closeKey);
        check(m_projects.documents.size()==2 && m_projects.selected==aId && (!m_pianoRollFrame || !m_pianoRollFrame->isVisible()),
              "Ctrl+W closes the active editor before its project tab");
    }
    const auto clipsBeforeRecording = a->project().findTrack(track)->clips.size();
    a->pause(); a->seekSeconds(14.);
    check(a->startRecording(track), "start MIDI recording in A");
    check(selectProjectTab(bId) && b->undoDepth()==undoB, "independent undo survives repeated view switches");
    check(a->isRecording() && !b->isRecording() &&
          m_midiInput->injectMessageForTest(QByteArray::fromHex("903c64")), "hardware MIDI still reaches recording A while B is selected");
    audio::AudioBuffer input(2, 256), output(2, 256);
    for (int block=0; block<24; ++block) a->processDeviceBlockForTest(input, output, 256);
    m_midiInput->injectMessageForTest(QByteArray::fromHex("803c00"));
    bool loadingVisible = false;
    QTimer::singleShot(0, this, [&] {
        refreshProjectTabs();
        loadingVisible = !m_projectPower->isEnabled() && m_projectPower->text().isEmpty() &&
            !m_projectPower->icon().isNull() && m_projectPower->accessibleName()==tr("Switching engine…");
    });
    toggleProjectAudio();
    check(loadingVisible, "UI refresh cannot erase the busy indicator or re-enable engine switching");
    check(m_projects.audio.active()==b && !b->isPlaying() && a->documentOnly(), "power action hands off audio without starting playback");
    check(m_projectPower->toolButtonStyle()==Qt::ToolButtonIconOnly && m_projectPower->isChecked() &&
          !m_projectPower->toolTip().isEmpty(), "engine power is an accessible icon with explicit on/off state");
    check(m_midiInput->checkProjectHandoffForTest(a), "hardware port survives handoff while stale queued events and detached input are discarded");
    const auto* recorded = a->project().findTrack(track);
    check(!a->isRecording() && recorded->clips.size()==clipsBeforeRecording+1 && !recorded->clips.back().notes.empty() &&
          b->undoDepth()==undoB, "engine handoff commits the recorded take only in A");
    a->seekSeconds(3.);
    selectProjectTab(aId);
    check(m_projects.audio.active()==b, "returning to A does not steal engine");
    const auto patternA = a->addTrack(daw::TrackKind::Pattern, "Pattern A");
    const auto patternClipA = a->addPatternClip(patternA, 0., 4.);
    openPatternClip(QString::fromStdString(patternA), QString::fromStdString(patternClipA));
    check(m_patternWindow && m_patternWindow->patternId()==QString::fromStdString(patternA), "open Pattern in first tab");
    selectProjectTab(bId);
    const auto patternB = b->addTrack(daw::TrackKind::Pattern, "Pattern B");
    const auto patternClipB = b->addPatternClip(patternB, 0., 4.);
    openPatternClip(QString::fromStdString(patternB), QString::fromStdString(patternClipB));
    check(m_patternWindow && m_patternWindow->patternId()==QString::fromStdString(patternB), "new rack binds Pattern editor to second document");
    selectProjectTab(aId);
    check(m_patternWindow && m_mixer->rackMode() && m_patternWindow->patternId()==QString::fromStdString(patternA) &&
          m_patternWindow->patternClipId()==QString::fromStdString(patternClipA), "Pattern and rack mode survive tab switches");
    m_timeline->selectClips({{QString::fromStdString(track), QString::fromStdString(clip)}});
    QMimeData dragData; dragData.setData("application/x-vlt-project-transfer", "1");
    const auto hover = m_projectTabs->tabRect(1).center();
    QDragEnterEvent enter(hover, Qt::CopyAction, &dragData, Qt::LeftButton, Qt::NoModifier);
    QDragMoveEvent move(hover, Qt::CopyAction, &dragData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_projectTabs, &enter); QApplication::sendEvent(m_projectTabs, &move);
    QEventLoop hoverWait; QTimer::singleShot(560, &hoverWait, &QEventLoop::quit); hoverWait.exec();
    check(m_projects.selected==bId && m_projects.audio.active()==b, "500 ms drag hover switches view without changing audio owner");
    const auto dropUndo = b->undoDepth(), sourceUndo = a->undoDepth();
    const QPoint dropPoint(210, m_timeline->laneCentreForTest(0));
    QDragEnterEvent arrangementEnter(dropPoint, Qt::CopyAction, &dragData, Qt::LeftButton, Qt::NoModifier);
    QDropEvent arrangementDrop(dropPoint, Qt::CopyAction, &dragData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(m_timeline, &arrangementEnter); QApplication::sendEvent(m_timeline, &arrangementDrop);
    check(arrangementDrop.isAccepted() && b->undoDepth()==dropUndo+1 && a->undoDepth()==sourceUndo &&
          m_projects.audio.active()==b, "dropping a cross-project copy creates one destination undo without altering the source");
    QTemporaryDir files;
    check(files.isValid(), "temporary project directory");
    selectProjectTab(aId); m_projectPath=files.path()+"/A.vlt"; m_dirty=true;
    selectProjectTab(bId); m_projectPath=files.path()+"/B.vlt"; m_dirty=true;
    check(saveAllProjectTabs() && !m_dirty && !m_projects.find(aId)->dirty && a->loadedPluginCount()==0,
          "Save All saves every changed document without powering dormant projects");
    check(openProjectPath(files.path()+"/A.vlt") && m_projects.selected==aId && m_projects.documents.size()==2,
          "opening an existing normalized path selects its tab");
    const auto cancelDialog = [] {
        for (auto* widget : QApplication::topLevelWidgets()) if (auto* box=qobject_cast<QMessageBox*>(widget)) {
            if (auto* cancel=box->button(QMessageBox::Cancel)) cancel->click();
            else box->accept();
        }
    };
    QTimer::singleShot(30, this, cancelDialog);
    check(!doSave(files.path()+"/B.vlt") && m_projectPath.endsWith("A.vlt"), "Save As cannot overwrite another open document");
    m_dirty=true; m_projects.find(bId)->dirty=true;
    {
        const QScopedValueRollback<bool> interactiveClose(m_persistGeometry, true);
        QTimer::singleShot(30, this, cancelDialog);
        check(!closeProjectTabs({aId,bId}) && m_projects.documents.size()==2 && m_projects.selected==aId,
              "cancel mass close preserves all tabs and original selection");
        QTimer::singleShot(30, this, cancelDialog);
        check(!setProjectTabsEnabled(false) && m_projectTabsEnabled &&
              QSettings().value("projects/tabsEnabled").toBool() && m_projects.documents.size()==2 && m_projects.selected==aId,
              "cancel disabling tabs preserves preference, documents and selection");
    }
    daw::recovery::RecoveryJournal recovery;
    check(recovery.start((files.path()+"/recovery").toStdString(), "test", std::chrono::milliseconds(0)), "workspace recovery writer");
    auto checkpoint = projectController().captureRecoverySnapshot(0); appendWorkspaceRecovery(checkpoint);
    recovery.requestWrite(std::move(checkpoint)); recovery.flush();
    daw::recovery::SessionInfo source; source.directory = recovery.sessionDir();
    QString recoveryError;
    m_projectTabsEnabled = false; QSettings().setValue("projects/tabsEnabled", false);
    check(restoreProjectWorkspace(source, &recoveryError), "restore all recovery documents transactionally");
    if (!recoveryError.isEmpty()) std::fprintf(stderr, "%s\n", recoveryError.toUtf8().constData());
    check(m_projects.documents.size()==2 && !m_projects.audio.active() && projectController().documentOnly(),
          "recovery keeps every document open with engines off");
    check(m_projectTabsEnabled && m_projectTabStrip->isVisible(), "multi-project recovery exposes all restored documents");
    check(!pianoA && !pianoB && !notesA, "recovery disposes outgoing document editors before their controllers");
    check(std::abs(projectController().positionSeconds()-3.)<.01 && std::abs(m_timeline->pixelsPerSecondForTest()-137.)<.001 && !m_selection.clips().empty(),
          "recovery restores cursor, zoom and selection in the selected document");
    recovery.stop();
    for (int i=0;i<5;++i) { addProjectTab(); projectController().setProjectName("Project with a very long title " + std::to_string(i)); }
    refreshProjectTabs(); QApplication::processEvents();
    check(m_projectTabs->usesScrollButtons() && m_projectTabs->count()==7, "overflow and long project titles");
    const auto maximumTabsWidth = m_projectTabs->maximumWidth();
    m_projectTabs->setMaximumWidth(220); QApplication::processEvents();
    int scrollButtons = 0;
    for (const auto* button : m_projectTabs->findChildren<QToolButton*>())
        if (button->isVisible() && button->arrowType()!=Qt::NoArrow) ++scrollButtons;
    auto* overflow = findChild<QToolButton*>("ProjectTabOverflow");
    if (overflow && overflow->menu()) QMetaObject::invokeMethod(overflow->menu(), "aboutToShow", Qt::DirectConnection);
    check(scrollButtons==2 && overflow && overflow->menu() && overflow->menu()->actions().size()==7,
          "narrow tab strip exposes scroll buttons and every document in the overflow menu");
    m_projectTabs->setMaximumWidth(maximumTabsWidth); QApplication::processEvents();
    const auto selectedTab = m_projectTabs->currentIndex();
    const auto selectedBounds = m_projectTabs->tabRect(selectedTab);
    auto* stateIcon = m_projectTabs->tabButton(selectedTab, QTabBar::LeftSide);
    auto* closeIcon = qobject_cast<QToolButton*>(m_projectTabs->tabButton(selectedTab, QTabBar::RightSide));
    check(stateIcon && closeIcon && selectedBounds.contains(stateIcon->geometry()) &&
          selectedBounds.contains(closeIcon->geometry()) && closeIcon->iconSize()==QSize(16,16),
          "status and close icons fit fully inside the selected tab at the current DPI");
    if (!shots.isEmpty()) { QDir().mkpath(shots); check(grab().save(shots+"/project-tabs.png"), "save tab screenshot"); }
    {
        ui::ProjectLoadDialog progress(this, QString::fromUtf8("127 158 bpm @frezzy"));
        progress.stage(ui::ProjectLoadDialog::tr("Preparing project…"), false);
        progress.show(); QApplication::processEvents();
        bool reentered = false;
        QTimer::singleShot(0, &progress, [&reentered] { reentered = true; });
        progress.plugin("Modular V3", 3, 11);
        check(!reentered && progress.labelText().contains("Modular V3"),
              "plugin loading status paints without dispatching pending model callbacks");
        if (!shots.isEmpty()) check(progress.grab().save(shots+"/project-loading.png"), "save loading window screenshot");
        QApplication::processEvents();
    }
    // Close clean documents without interactive prompts in the headless run.
    m_dirty = false; for (auto& s : m_projects.documents) s->dirty=false;
    QList<ProjectId> ids; for (const auto& s : m_projects.documents) ids.append(s->id);
    check(closeProjectTabs(ids), "close all documents");
    check(m_projects.documents.size()==1 && projectController().documentOnly() && !m_projects.audio.active(), "last close creates one empty inactive document");
    check(!m_midiInput->injectMessageForTest(QByteArray::fromHex("904364")), "MIDI remains safely detached after closing every document");
    {
        daw::EngineController document; document.initializeDocumentOnly();
        const auto channel = document.addTrack(daw::TrackKind::Audio, "Device status");
        const auto device = document.addInsert(channel, daw::plugins::equalizer::EqualizerInstance::staticDescriptor());
        ChannelStrip strip(&document, QString::fromStdString(channel), false);
        QToolButton* label = nullptr;
        for (auto* button : strip.findChildren<QToolButton*>())
            if (button->property("pluginSlotId").toString()==QString::fromStdString(device)) label=button;
        strip.refreshPluginHealth();
        check(label && !label->property("missing").toBool() && label->property("engineOff").toBool(),
              "parked plugin is shown as engine off rather than missing");
        check(bool(document.activateAudio(m_projects.audio.configuration(), false)), "activate status-test device");
        // Simulate a strip retained from the old runtime, including its red missing flag.
        if (label) label->setProperty("missing", true);
        strip.refreshPluginHealth();
        check(label && !label->property("missing").toBool() && !label->property("engineOff").toBool() &&
              document.hasInsert(channel, device), "activation clears stale missing status without rebuilding the strip");
        document.deactivateAudio(); strip.refreshPluginHealth();
        check(label && !label->property("missing").toBool() && label->property("engineOff").toBool(),
              "power off refreshes retained plugin labels immediately");
        const auto openPath = files.path()+"/AutoEngine.vlt";
        check(bool(document.saveProject(openPath.toStdString())), "prepare project for automatic engine start");
        check(openProjectPath(openPath) && m_projects.audio.active()==&projectController() && !projectController().isPlaying(),
              "opening a project enables its engine without autoplay");
    }
    {
        const auto sounding = m_projects.selected;
        check(addProjectTab(), "add context-menu destination");
        const auto selected = m_projects.selected;
        int clickedTab = -1;
        for (int i=0; i<m_projectTabs->count(); ++i) if (m_projectTabs->tabData(i).toString()==sounding) clickedTab=i;
        const auto menuPosition = m_projectTabs->tabRect(clickedTab).center();
        QContextMenuEvent rightClick(QContextMenuEvent::Mouse, menuPosition, m_projectTabs->mapToGlobal(menuPosition));
        QApplication::sendEvent(m_projectTabs, &rightClick);
        auto* popup = m_projectTabs->findChild<QMenu*>("ProjectTabContextMenu");
        check(popup && popup->isVisible() && m_projects.selected==selected && m_projects.audible() && m_projects.audible()->id==sounding,
              "right click opens the clicked tab menu without selecting it or switching audio");
        if (popup) { popup->close(); QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
        std::unique_ptr<QMenu> menu(createProjectTabMenu(sounding));
        check(m_projects.selected==selected && m_projects.audible() && m_projects.audible()->id==sounding,
              "opening context menu does not change selection or audio owner");
        check(menu->findChild<QAction*>("project.tabSave") && menu->findChild<QAction*>("project.tabSaveAs") &&
              menu->findChild<QAction*>("project.tabCloseOthers") && menu->findChild<QAction*>("project.tabCloseAll"),
              "tab context menu exposes save and close commands");
        if (!shots.isEmpty()) {
            menu->popup(m_projectTabs->mapToGlobal(QPoint(10, m_projectTabs->height())));
            QApplication::processEvents();
            check(menu->grab().save(shots+"/project-tab-menu.png"), "save tab menu screenshot");
            menu->hide();
        }
        menu->findChild<QAction*>("project.tabEngine")->trigger();
        check(!m_projects.audio.active() && m_projects.selected==selected,
              "context engine command targets the clicked tab and preserves the viewed tab");
        menu->findChild<QAction*>("project.tabEngine")->trigger();
        openSettings(SettingsWindow::kInterfaceTab);
        ensurePerformanceWindow();
        QPointer<SettingsWindow> ownerSettings = m_settingsWindow;
        QPointer<PerformanceWindow> ownerPerformance = m_performanceWindow;
        m_dirty=false; for(auto& session:m_projects.documents) session->dirty=false;
        menu->findChild<QAction*>("project.tabClose")->trigger();
        check(!m_projects.find(sounding) && m_projects.selected==selected,
              "context close targets the clicked background tab");
        check(!ownerSettings && !ownerPerformance && !m_settingsWindow && !m_performanceWindow && !m_projects.audio.active(),
              "closing a clean background audio owner retires its controller-bound settings and performance windows");
    }
    m_dirty=false; for(auto& session:m_projects.documents) session->dirty=false;
    const auto kept = m_projects.selected;
    check(setProjectTabsEnabled(false) && m_projects.documents.size()==1 && m_projects.selected==kept &&
          !m_projectTabStrip->isVisible() && !QSettings().value("projects/tabsEnabled", true).toBool(),
          "one-project mode closes other documents, hides the entire strip and persists");
    check(addProjectTab() && m_projects.documents.size()==1 && !m_projects.find(kept), "New replaces the one document");
    const auto blank = m_projects.selected;
    {
        const QScopedValueRollback<bool> interactiveClose(m_persistGeometry, true);
        m_dirty=true;
        QTimer::singleShot(30, this, cancelDialog);
        check(!addProjectTab() && m_projects.documents.size()==1 && m_projects.selected==blank,
              "cancel New preserves the single unsaved document");
        m_dirty=false; m_projects.current().dirty=false;
    }
    const auto singleOpen = files.path()+"/AutoEngine.vlt";
    check(openProjectPath(singleOpen) && m_projects.documents.size()==1 && !m_projects.find(blank) &&
          m_projects.audio.active()==&projectController() && !projectController().isPlaying(),
          "Open replaces one document and enables its engine without autoplay");
    const auto opened = m_projects.selected;
    check(openProjectPath(singleOpen) && m_projects.documents.size()==1 && m_projects.selected==opened,
          "opening the same path preserves the single document");
    {
        QTimer dismissError;
        connect(&dismissError, &QTimer::timeout, this, cancelDialog);
        dismissError.start(30);
        check(!openProjectPath(files.path()+"/Missing.vlt") && m_projects.documents.size()==1 && m_projects.selected==opened &&
              m_projects.audio.active()==&projectController(), "failed Open preserves the single document and its engine");
    }
    QApplication::processEvents();
    if (!shots.isEmpty()) check(grab().save(shots+"/single-project.png"), "save one-project screenshot");
    openSettings(SettingsWindow::kInterfaceTab);
    auto* preference = m_settingsWindow->findChild<QCheckBox*>("ProjectTabsEnabled");
    check(preference && !preference->isChecked(), "settings exposes the persisted project-tabs preference");
    QApplication::processEvents();
    if (!shots.isEmpty()) check(m_settingsWindow->grab().save(shots+"/project-tabs-settings.png"), "save project-tabs preference screenshot");
    if (preference) preference->setChecked(true);
    QApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    check(m_projectTabsEnabled && m_projectTabStrip->isVisible(), "settings enables project tabs immediately");
    const auto settingsOwner = m_projects.selected;
    check(addProjectTab(), "add project for settings close transaction");
    const auto settingsSelection = m_projects.selected;
    openSettings(SettingsWindow::kInterfaceTab);
    {
        const QScopedValueRollback<bool> interactiveClose(m_persistGeometry, true);
        m_projects.find(settingsOwner)->dirty=true;
        QTimer::singleShot(30, this, cancelDialog);
        m_settingsWindow->findChild<QCheckBox*>("ProjectTabsEnabled")->setChecked(false);
        QApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        check(m_projectTabsEnabled && m_projects.documents.size()==2 && m_projects.selected==settingsSelection &&
              m_settingsWindow && m_settingsWindow->findChild<QCheckBox*>("ProjectTabsEnabled")->isChecked(),
              "cancel from settings restores the checkbox and safely reopens controller-bound settings");
    }
    m_dirty=false; for(auto& session:m_projects.documents) session->dirty=false;
    if (m_settingsWindow) {
        m_settingsWindow->findChild<QCheckBox*>("ProjectTabsEnabled")->setChecked(false);
        QApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }
    check(!m_projectTabsEnabled && m_projects.documents.size()==1 && m_projects.selected==settingsSelection && m_settingsWindow &&
          !m_settingsWindow->findChild<QCheckBox*>("ProjectTabsEnabled")->isChecked(),
          "disabling from settings safely closes the background audio owner and restores settings");
    setProjectTabsEnabled(true);
    delete m_settingsWindow; m_settingsWindow=nullptr;
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto buttonsBefore = m_projectTabs->findChildren<QToolButton*>("ProjectTabClose").size();
    bool retired = true;
    for (int cycle=0;cycle<8;++cycle) {
        addProjectTab();
        const auto id = m_projects.selected;
        QPointer<QWidget> left = m_projectTabs->tabButton(m_projectTabs->count()-1, QTabBar::LeftSide);
        QPointer<QWidget> right = m_projectTabs->tabButton(m_projectTabs->count()-1, QTabBar::RightSide);
        closeProjectTabs({id});
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        retired &= !left && !right;
    }
    const auto buttonsAfter = m_projectTabs->findChildren<QToolButton*>("ProjectTabClose").size();
    if (!retired || buttonsAfter != buttonsBefore)
        std::fprintf(stderr, "Tab widget lifetime: retired=%d, before=%lld, after=%lld\n", int(retired), qlonglong(buttonsBefore), qlonglong(buttonsAfter));
    check(retired && buttonsAfter==buttonsBefore,
          "repeated open/close releases tab buttons without accumulating hidden widgets");
    return failures == 0;
}
