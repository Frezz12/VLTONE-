#include "MainWindow.hpp"
#include "MenuActions.hpp"
#include "ShortcutManager.hpp"
#include "TimelineWidget.hpp"
#include "TrackListWidget.hpp"
#include "TransportBar.hpp"
#include "AutomationEditorWindow.hpp"
#include "PianoRollWindow.hpp"
#include <QPointer>
#include <QCoreApplication>
#include <QDir>
#include <QMenu>
#include <cstdio>

bool MainWindow::checkCommandMenusForTest() {
    bool ok = true;
    const auto check = [&](bool pass, const char* label) {
        std::fprintf(stderr, "%s command menus: %s\n", pass ? "PASS" : "FAIL", label);
        ok &= pass;
    };
    const auto openMenu = [this](const char* id) -> QMenu* {
        const auto* command = m_shortcuts->command(QString::fromLatin1(id));
        QMenu* menu = command ? command->action->menu() : nullptr;
        if (menu) QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection);
        return menu;
    };
    const auto find = [](QMenu* menu, const char* context, const char* source) -> QAction* {
        if (!menu) return nullptr;
        const QString label = QCoreApplication::translate(context, source);
        for (auto* action : menu->actions())
            if (action->text().section('\t', 0, 0) == label) return action;
        return nullptr;
    };
    const auto snapshot = [](QMenu* menu, const QString& name) {
        const QString directory = qEnvironmentVariable("DAW_MENU_SCREENSHOTS");
        if (!menu || directory.isEmpty()) return;
        QDir().mkpath(directory);
        menu->ensurePolished();
        menu->adjustSize();
        menu->grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
    };
    const auto original = m_controller.project();
    daw::ProjectModel project;
    daw::TrackModel track;
    track.id = daw::newUuid(); track.name = "Menu fixture"; track.kind = daw::TrackKind::Midi;
    daw::ClipModel first;
    first.id = daw::newUuid(); first.kind = daw::ClipKind::Midi;
    first.startSeconds = 300.0; first.durationSeconds = 4.0;
    first.name = "Hidden selected clip";
    daw::TakeModel take;
    take.id = daw::newUuid(); take.name = "Take 1"; take.lengthSeconds = 4.0;
    first.takes = {take};
    auto second = first;
    second.id = daw::newUuid(); second.name = "Overlapping clip"; second.takes.clear();
    track.clips = {first, second}; project.tracks = {track};
    m_controller.restoreProject(project, "Command menu test");
    syncViews();
    const auto trackId = QString::fromStdString(track.id);
    m_timeline->selectClips({{trackId, QString::fromStdString(first.id)}});
    const double scrollBefore = m_timeline->horizontalScrollForTest();
    const int verticalBefore = m_timeline->verticalScroll();
    QMenu* selection = openMenu("edit.selectionActions");
    check(selection && find(selection, "TimelineWidget", "Mute Clip"),
          "selected clip has real menu items even when hidden and overlapped");
    check(m_timeline->horizontalScrollForTest() == scrollBefore &&
          m_timeline->verticalScroll() == verticalBefore,
          "opening actions leaves the viewport unchanged");
    if (selection) {
        const int children = selection->findChildren<QMenu*>().size();
        for (int i = 0; i < 3; ++i) openMenu("edit.selectionActions");
        check(selection->findChildren<QMenu*>().size() == children,
              "reopening does not accumulate submenus or handlers");
        bool noDuplicateKeys = true;
        for (auto* action : selection->findChildren<QAction*>())
            noDuplicateKeys &= action->shortcuts().isEmpty();
        check(noDuplicateKeys, "context actions do not register duplicate global shortcuts");
    }
    snapshot(selection, QStringLiteral("selection"));
    QAction* mute = find(selection, "TimelineWidget", "Mute Clip");
    const int undoBefore = m_controller.undoDepth();
    if (mute) mute->trigger();
    const auto* changed = m_controller.project().findTrack(track.id);
    check(changed && changed->clips[0].muted && !changed->clips[1].muted &&
          m_controller.undoDepth() == undoBefore + 1,
          "menu targets selected identity and applies exactly one undoable edit");
    selection = openMenu("edit.selectionActions");
    auto* takes = find(selection, "TimelineWidget", "Takes");
    QMenu* takeMenu = takes && takes->menu() && !takes->menu()->actions().isEmpty()
        ? takes->menu()->actions().front()->menu() : nullptr;
    snapshot(takeMenu, QStringLiteral("take"));
    auto* muteTake = find(takeMenu, "TimelineWidget", "Mute Take");
    if (muteTake) muteTake->trigger();
    changed = m_controller.project().findTrack(track.id);
    check(muteTake && changed && changed->clips[0].takes[0].muted,
          "take-row actions are available from the menu bar");
    m_trackList->setSelectedTrack(trackId);
    auto* trackMenu = openMenu("track.actions");
    snapshot(trackMenu, QStringLiteral("track"));
    auto* duplicate = find(trackMenu, "TrackListWidget", "Duplicate Track (without plugins)");
    const auto tracksBefore = m_controller.project().tracks.size();
    if (duplicate) duplicate->trigger();
    check(duplicate && m_controller.project().tracks.size() == tracksBefore + 1,
          "track context-only duplication is reachable from the menu bar");

    m_trackList->setSelectedTrack(trackId);
    selectTrackFromHeader(trackId);
    const double oldPosition = m_controller.positionSeconds();
    m_controller.seekSeconds(12.0);
    const auto clipsBefore = m_controller.project().findTrack(track.id)->clips.size();
    m_shortcuts->invoke(QStringLiteral("edit.addMidiClip"));
    changed = m_controller.project().findTrack(track.id);
    check(changed && changed->clips.size() == clipsBefore + 1 &&
          changed->clips.back().kind == daw::ClipKind::Midi &&
          std::abs(changed->clips.back().startSeconds - 12.0) < 0.001,
          "new MIDI clips use the selected track and playhead");
    m_controller.seekSeconds(oldPosition);
    {
        PianoRollWindow roll(&m_controller);
        QMenu actions;
        roll.populateActionsMenu(actions);
        QPointer<QAction> leaf;
        if (!actions.actions().isEmpty() && actions.actions().front()->menu()) {
            for (auto* action : actions.actions().front()->menu()->actions()) {
                if (!action->isSeparator() && !action->menu()) { leaf = action; break; }
            }
        }
        ui::clearMenu(actions);
        roll.populateActionsMenu(actions);
        check(leaf && !actions.isEmpty(),
              "reopening editor menus preserves the editor-owned actions");
    }

    // Rebuild for an automation clip; the point's menu is intentionally lazy.
    project.tracks[0].clips = {second};
    auto& curve = project.tracks[0].clips[0];
    curve.kind = daw::ClipKind::Automation;
    curve.automation.points = {{0.0, 0.2}, {4.0, 0.8}};
    m_controller.restoreProject(project, "Automation menu test"); syncViews();
    m_timeline->selectClips({{trackId, QString::fromStdString(second.id)}});
    selection = openMenu("edit.selectionActions");
    auto* automation = find(selection, "TimelineWidget", "Automation");
    auto* points = find(automation ? automation->menu() : nullptr, "TimelineWidget", "Points");
    QMenu* pointMenu = points && points->menu() && !points->menu()->actions().isEmpty()
        ? points->menu()->actions().front()->menu() : nullptr;
    if (pointMenu) QMetaObject::invokeMethod(pointMenu, "aboutToShow", Qt::DirectConnection);
    auto* removePoint = find(pointMenu, "TimelineWidget", "Delete Point");
    if (removePoint) removePoint->trigger();
    changed = m_controller.project().findTrack(track.id);
    check(removePoint && changed && changed->clips[0].automation.points.size() == 1,
          "individual automation points are reachable without hit-testing");

    {
        AutomationEditorWindow editor(&m_controller, trackId, QString::fromStdString(second.id));
        QMenu actions;
        editor.populateActionsMenu(actions);
        auto* shape = find(&actions, "AutomationEditorWindow", "Shape");
        auto* invert = find(shape ? shape->menu() : nullptr, "AutomationEditorWindow", "Invert");
        const double before = m_controller.project().findTrack(track.id)->clips[0].automation.points[0].value;
        if (invert) invert->trigger();
        changed = m_controller.project().findTrack(track.id);
        check(invert && changed &&
              std::abs(changed->clips[0].automation.points[0].value - (1.0 - before)) < 0.001,
              "automation editor menu reuses the toolbar transform");
    }

    const int oldSecondary = m_transport->secondaryToolIndex();
    m_shortcuts->invoke(QStringLiteral("tool.secondaryStretch"));
    check(m_transport->secondaryToolIndex() == 6, "secondary Stretch command matches toolbar");
    m_shortcuts->invoke(QStringLiteral("tool.secondaryGlue"));
    check(m_transport->secondaryToolIndex() == 7, "secondary Glue command matches toolbar");
    m_transport->setSecondaryToolIndex(oldSecondary);
    const bool oldCounter = m_transport->positionShowsBars();
    const bool oldRuler = m_transport->showsBars();
    m_shortcuts->invoke(QStringLiteral("view.positionClock"));
    check(!m_transport->positionShowsBars() && m_transport->showsBars() == oldRuler,
          "counter format changes independently of the ruler");
    m_transport->setPositionDisplayBars(oldCounter);
    m_controller.restoreProject(original, "Restore command menu fixture");
    syncViews();
    m_timeline->selectClips({});
    return ok;
}
