#include "TrackOverviewPanel.hpp"
#include "MainWindow.hpp"
#include "TrackListWidget.hpp"
#include "InspectorWidget.hpp"
#include "FileBrowserPanel.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Theme.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include <QApplication>
#include <QDir>
#include <QDrag>
#include <QAbstractItemDelegate>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QTreeWidget>
#include <QSettings>
#include <QTimer>
#include <QQuickWindow>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QtTest/qtestspontaneevent.h>
#include <cstdio>
#include <array>
#include <cmath>

bool TrackOverviewPanel::checkForTest(const QString& screenshots) {
    bool ok = true;
    const auto check = [&](bool value, const char* label) {
        std::printf("%s Track overview: %s\n", value ? "PASS" : "FAIL", label);
        std::fflush(stdout);
        ok &= value;
        return value;
    };
    daw::EngineController controller;
    if (!controller.initialize(48000, 512, false)) return false;
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    project.tracks.clear(); project.invalidateStructure();
    const auto drums = controller.addFolder(true, "Drums");
    const auto kick = controller.addTrack(daw::TrackKind::Midi, "Kick");
    const auto snare = controller.addTrack(daw::TrackKind::Midi, "Snare");
    const auto percussion = controller.addFolder(false, "Percussion");
    const auto hats = controller.addTrack(daw::TrackKind::Midi, "Closed hats");
    const auto music = controller.addFolder(true, "Music");
    const auto bass = controller.addTrack(daw::TrackKind::Midi, "Sub bass");
    const auto lead = controller.addTrack(daw::TrackKind::Midi, "Glass lead");
    const auto vocals = controller.addTrack(daw::TrackKind::Audio, "Vocal textures");
    controller.moveTracks({kick, snare, percussion}, project.tracks.size(), drums);
    controller.moveTracks({hats}, project.tracks.size(), percussion);
    controller.moveTracks({bass, lead}, project.tracks.size(), music);
    const auto clip = controller.addMidiClip(kick, 1.0, 2.0);
    controller.setFolderExpanded(drums, false);
    TrackOverviewPanel panel(&controller);
    panel.resize(200, 560); panel.show(); QApplication::processEvents();
    const auto q = [](const std::string& id) { return QString::fromStdString(id); };
    const auto item = [&](const std::string& id) { return panel.m_items.value(q(id)); };
    check(panel.m_items.size() == 9 && item(kick)->parent() == item(drums) &&
          item(hats)->parent() == item(percussion), "complete nested hierarchy includes collapsed timeline tracks");
    check(item(drums)->isExpanded() && !project.findTrack(drums)->expanded,
          "navigator disclosure is independent from the arrangement");

    const auto clickIn = [&](TrackOverviewPanel& target, const std::string& id, Qt::KeyboardModifiers modifiers = Qt::NoModifier, int column = 0, int nameOffset = 55) {
        auto* rowItem = target.m_items.value(q(id));
        target.m_tree->scrollToItem(rowItem); QApplication::processEvents();
        const QRect row = target.m_tree->visualItemRect(rowItem);
        const QPoint pos(target.m_tree->columnViewportPosition(column) +
            (column == 0 ? nameOffset : target.m_tree->columnWidth(column) / 2), row.center().y());
        const QPoint global = target.m_tree->viewport()->mapToGlobal(pos);
        QMouseEvent down(QEvent::MouseButtonPress, pos, global, Qt::LeftButton, Qt::LeftButton, modifiers);
        QMouseEvent up(QEvent::MouseButtonRelease, pos, global, Qt::LeftButton, Qt::NoButton, modifiers);
        // Qt's range-selection anchor also reads the application's keyboard
        // state, which notify() updates only for spontaneous input events.
        QSpontaneKeyEvent::setSpontaneous(&down);
        QSpontaneKeyEvent::setSpontaneous(&up);
        qApp->notify(target.m_tree->viewport(), &down);
        qApp->notify(target.m_tree->viewport(), &up);
        QApplication::processEvents();
    };
    const auto click = [&](const std::string& id, Qt::KeyboardModifiers modifiers = Qt::NoModifier, int column = 0) {
        clickIn(panel, id, modifiers, column);
    };
    // Exercise the item-view press/drag path, including the last mouse move
    // and release delivered after a move rebuilds the model. Calling only
    // moveSelection() misses Qt falling back to rubber-band selection here.
    const auto dragIn = [&](TrackOverviewPanel& target, const QString& from, const QString& to, bool commit, bool intoFolder = true) {
        auto* tree = target.m_tree;
        auto* viewport = tree->viewport();
        const QPoint press(55, tree->visualItemRect(target.m_items.value(from)).center().y());
        const QPoint end(55, tree->visualItemRect(target.m_items.value(to)).center().y());
        auto* surface = target.window()->findChild<ui::graphics::WorkspaceSurface*>();
        const auto mouse = [&](QEvent::Type type, QPoint pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
            const auto global = viewport->mapToGlobal(pos);
            const auto local = surface ? surface->quickWindow()->mapFromGlobal(global) : pos;
            QMouseEvent event(type, local, global, button, buttons, Qt::NoModifier);
            QSpontaneKeyEvent::setSpontaneous(&event);
            qApp->notify(surface ? static_cast<QObject*>(surface->quickWindow()) : viewport, &event);
        };
        QTimer cancel;
        cancel.setSingleShot(true);
        QObject::connect(&cancel, &QTimer::timeout, tree, [] { QDrag::cancel(); });
        mouse(QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
        cancel.start(20);
        mouse(QEvent::MouseMove, press + QPoint(QApplication::startDragDistance() + 2, 0), Qt::NoButton, Qt::LeftButton);
        cancel.stop();
        // Native platforms may finish a synthetic drag immediately (there is
        // no physical button held). Commit the same command as the drop handler
        // before delivering its trailing pointer events.
        const auto* destination = target.m_controller->project().findTrack(to.toStdString());
        const bool moved = !commit || target.moveSelection(intoFolder ? to : q(destination->parentId),
            intoFolder ? target.afterSubtree(to) : target.m_controller->project().indexOf(to.toStdString()));
        mouse(QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        QApplication::processEvents();
        return moved;
    };
    click(drums);
    check(!item(drums)->isExpanded() && !project.findTrack(drums)->expanded,
          "folder name click collapses the navigator without changing the timeline");
    click(drums);
    check(item(drums)->isExpanded(), "folder name click expands the navigator");
    clickIn(panel, percussion, Qt::NoModifier, 0, 12);
    check(!item(percussion)->isExpanded(), "nested folder icon also toggles disclosure without an arrow");
    clickIn(panel, percussion, Qt::NoModifier, 0, 12);
    click(drums, Qt::ControlModifier);
    check(item(drums)->isExpanded(), "modified folder clicks preserve expansion for multi-selection");
    click(kick); click(snare, Qt::ControlModifier);
    check(panel.selectedTrackIds() == QStringList{q(kick), q(snare)}, "Ctrl-click selects multiple tracks");
    const auto beforeMute = controller.undoDepth();
    click(kick, Qt::NoModifier, 1);
    check(project.findTrack(kick)->muted && project.findTrack(snare)->muted &&
          controller.undoDepth() == beforeMute, "one M click applies to the selection without changing edit history");
    controller.setTracksMuted({std::array<std::string, 2>{kick, snare}}, false); panel.refresh();
    check(!project.findTrack(kick)->muted && !project.findTrack(snare)->muted, "group unmute restores both tracks");
    click(bass, Qt::NoModifier, 2);
    check(project.findTrack(bass)->soloed && panel.selectedTrackIds() == QStringList{q(kick), q(snare)},
          "a control outside the selection acts on its own row without stealing selection");
    controller.setTrackSoloed(bass, false); panel.refresh();
    click(kick); click(snare, Qt::ShiftModifier);
    check(panel.selectedTrackIds() == QStringList{q(kick), q(snare)}, "Shift-click selects a range");

    // Windows OLE reads physical mouse buttons and can wait indefinitely for
    // hardware input in a synthetic drag. Run these event regressions with
    // QT_QPA_PLATFORM=offscreen; native runs still cover all other interactions
    // and capture the actual GPU presentation.
    const bool syntheticDrags = QGuiApplication::platformName() == "offscreen";
    if (syntheticDrags) {
        click(kick);
        dragIn(panel, q(kick), q(snare), false);
        check(panel.selectedTrackIds() == QStringList{q(kick)}, "cancelled drag does not select the row under the pointer");
        click(kick);
        const auto beforePointerMove = controller.undoDepth();
        check(dragIn(panel, q(kick), q(music), true) && project.findTrack(kick)->parentId == music,
              "drag completion moves the track into the destination folder");
        check(panel.selectedTrackIds() == QStringList{q(kick)} &&
              panel.m_tree->currentItem() == item(kick), "drop and final mouse release keep only the moved track selected");
        if (controller.undoDepth() > beforePointerMove) { controller.undo(); panel.refresh(); }
        click(snare);
        check(panel.selectedTrackIds() == QStringList{q(snare)}, "the next click starts a fresh selection after a drag");
        click(kick); click(snare, Qt::ControlModifier);
        const auto beforeGroupDrag = controller.undoDepth();
        check(dragIn(panel, q(kick), q(music), true) &&
              panel.selectedTrackIds() == QStringList{q(kick), q(snare)},
              "drag completion preserves a deliberate multi-selection without adding crossed rows");
        if (controller.undoDepth() > beforeGroupDrag) { controller.undo(); panel.refresh(); }
        click(snare);
        const auto beforeReorder = controller.undoDepth();
        check(dragIn(panel, q(snare), q(kick), true, false) && project.indexOf(snare) < project.indexOf(kick) &&
              panel.selectedTrackIds() == QStringList{q(snare)}, "reordering siblings keeps the moved track selected");
        if (controller.undoDepth() > beforeReorder) { controller.undo(); panel.refresh(); }
    } else {
        std::puts("SKIP synthetic drag events on a native platform; covered by QT_QPA_PLATFORM=offscreen");
    }
    click(kick); click(snare, Qt::ShiftModifier);

    const auto beforeMove = controller.undoDepth();
    check(panel.moveSelection(q(music), panel.afterSubtree(q(music))) &&
          project.findTrack(kick)->parentId == music && project.findTrack(snare)->parentId == music &&
          project.findTrack(kick)->outputBusId == music && controller.undoDepth() == beforeMove + 1,
          "multi-track move updates routing and records one undo");
    controller.undo(); panel.refresh();
    check(project.findTrack(kick)->parentId == drums && project.findTrack(snare)->parentId == drums &&
          project.findTrack(kick)->outputBusId == drums, "undo restores parents, order and bus routing");
    controller.redo(); panel.refresh(); controller.undo(); panel.refresh();

    panel.setSelectedTracks({q(percussion), q(hats)});
    check(panel.moveSelection(q(music), panel.afterSubtree(q(music))) &&
          project.findTrack(hats)->parentId == percussion, "moving a selected folder and child preserves nesting");
    controller.undo(); panel.refresh();
    panel.setSelectedTracks({q(drums)});
    check(!panel.moveSelection(q(percussion), panel.afterSubtree(q(percussion))) &&
          !panel.moveSelection(q(drums), panel.afterSubtree(q(drums))), "self and descendant drops are rejected");
    check(!panel.moveSelection(q(vocals), panel.afterSubtree(q(vocals))), "an ordinary audio track cannot become a folder");

    panel.setSelectedTracks({q(kick), q(snare)});
    controller.setTracksEditLocked({kick, snare}, true); panel.refresh();
    const auto lockedRevision = controller.projectRevision();
    controller.setClipStartSeconds(kick, clip, 8);
    controller.removeClip(kick, clip);
    controller.addMidiClip(kick, 12, 2);
    controller.removeTrack(kick);
    controller.renameTrack(kick, "Should not rename");
    check(!panel.moveSelection(q(music), panel.afterSubtree(q(music))) &&
          project.findTrack(kick) && project.findTrack(kick)->name == "Kick" &&
          project.findTrack(kick)->clips.size() == 1 && project.findTrack(kick)->clips.front().startSeconds == 1 &&
          controller.projectRevision() == lockedRevision, "locks reject content edits, deletion, renaming and track moves without history changes");
    check(!controller.moveTracks({drums}, project.tracks.size(), {}) &&
          controller.packIntoFolder({kick, lead}).empty(), "locked descendants protect the folder and reject partial packing");
    controller.setTrackMuted(kick, true);
    check(project.findTrack(kick)->muted, "playback controls remain available on protected tracks");
    controller.setTrackMuted(kick, false);
    controller.undo(); panel.refresh();
    check(!controller.isTrackEditLocked(kick) && !controller.isTrackEditLocked(snare), "group unlock through undo restores editing");
    controller.redo(); panel.refresh();
    std::string bytes; daw::ProjectModel restored;
    check(daw::ProjectSerializer::serializeDocument(project, bytes).isOk() &&
          daw::ProjectSerializer::deserializeDocument(restored, bytes).isOk() && restored.findTrack(kick)->editLocked,
          "edit locks survive document serialization");
    controller.setTracksEditLocked({kick, snare}, false);
    controller.setTracksEditLocked({drums}, true); panel.refresh();
    check(controller.isTrackEditLocked(hats) && item(hats)->checkState(3) == Qt::Checked,
          "nested tracks visibly inherit folder protection");
    panel.setSelectedTracks({q(bass)});
    check(!panel.moveSelection(q(drums), panel.afterSubtree(q(drums))), "locked destinations reject incoming tracks");
    controller.setTracksEditLocked({drums}, false); panel.refresh();

    panel.setSelectedTracks({q(kick), q(snare)});
    QMenu packMenu; panel.populateMenu(packMenu);
    packMenu.findChild<QAction*>("overview.packSumming")->trigger();
    const auto packed = panel.selectedTrackIds().value(0).toStdString();
    if (!check(project.findTrack(packed) && project.findTrack(packed)->summing &&
               project.findTrack(kick)->parentId == packed && project.findTrack(snare)->parentId == packed,
               "context menu packs the selection into a summing folder")) return false;
    QMenu convert; panel.populateMenu(convert);
    convert.findChild<QAction*>("overview.summing")->trigger();
    check(!project.findTrack(packed)->summing && project.findTrack(kick)->outputBusId == drums,
          "conversion to a regular folder restores outer bus routing");
    controller.undo(); controller.undo(); panel.refresh();

    panel.setSelectedTracks({q(vocals)});
    panel.m_search->setText("closed");
    check(!item(drums)->isHidden() && !item(percussion)->isHidden() && !item(hats)->isHidden() &&
          item(vocals)->isHidden() && panel.selectedTrackIds().isEmpty(),
          "search retains ancestor context and clears hidden selections");
    panel.m_search->setText("nothing matches this");
    check(item(drums)->isHidden() && item(music)->isHidden(), "empty search result is explicit");
    panel.m_search->clear();
    panel.setSelectedTracks({q(bass)});
    QMenu moveMenu; panel.populateMenu(moveMenu);
    moveMenu.findChild<QAction*>("overview.moveRoot")->trigger();
    check(project.findTrack(bass)->parentId.empty(), "context menu moves a track to the top level");
    controller.undo(); panel.refresh();

    const QString previousTheme = ThemeManager::instance().themeId();
    const auto luminance = [](const QColor& color) {
        const auto channel = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
        return .2126 * channel(color.redF()) + .7152 * channel(color.greenF()) + .0722 * channel(color.blueF());
    };
    for (const auto& theme : ThemeManager::instance().presets()) {
        ThemeManager::instance().setThemeId(theme.id, false);
        bool readable = true;
        double minimumContrast = 100;
        for (bool focused : {false, true}) {
            QImage row(220, 24, QImage::Format_ARGB32_Premultiplied); row.fill(th().surface);
            QStyleOptionViewItem option;
            option.initFrom(panel.m_tree); option.rect = row.rect(); option.widget = panel.m_tree;
            option.state |= QStyle::State_Selected;
            if (focused) option.state |= QStyle::State_HasFocus | QStyle::State_Active;
            else option.state &= ~(QStyle::State_HasFocus | QStyle::State_Active);
            QPainter painter(&row);
            panel.m_tree->itemDelegate()->paint(&painter, option, panel.m_tree->indexFromItem(item(kick)));
            painter.end();
            const QColor background = row.pixelColor(210, 12);
            const double a = luminance(background), b = luminance(th().textPrimary);
            const double contrast = (std::max(a, b) + .05) / (std::min(a, b) + .05);
            minimumContrast = std::min(minimumContrast, contrast);
            readable &= contrast >= 4.5 && background != th().accent;
        }
        std::printf("Track overview contrast %s: %.2f:1\n", theme.id.toUtf8().constData(), minimumContrast);
        check(readable, "focused and inactive selected names keep normal-text contrast in every preset");
    }
    ThemeManager::instance().setThemeId(previousTheme, false);

    panel.resize(80, 560); QApplication::processEvents();
    panel.setSelectedTracks({q(kick), q(snare)});
    check(panel.width() == 180 && panel.minimumWidth() == 180 && panel.m_tree->columnWidth(0) >= 100 &&
          !panel.m_tree->isColumnHidden(1),
          "180 px minimum keeps names and per-row controls visible");
    click(kick, Qt::NoModifier, 3);
    check(controller.isTrackEditLocked(kick) && controller.isTrackEditLocked(snare),
          "row lock button protects the full selection at minimum width");
    click(kick, Qt::NoModifier, 3);
    QMenu searchMenu; panel.populateMenu(searchMenu);
    searchMenu.findChild<QAction*>("overview.find")->trigger(); QApplication::processEvents();
    check(panel.m_search->hasFocus(), "header menu focuses track search");
    panel.m_tree->setFocus();
    const auto findKey = QKeySequence(QKeySequence::Find)[0];
    QKeyEvent findOverride(QEvent::ShortcutOverride, findKey.key(), findKey.keyboardModifiers());
    QApplication::sendEvent(panel.m_tree, &findOverride);
    QKeyEvent find(QEvent::KeyPress, findKey.key(), findKey.keyboardModifiers());
    QApplication::sendEvent(panel.m_tree, &find);
    check(findOverride.isAccepted() && panel.m_search->isVisible() && panel.m_search->hasFocus(),
          "navigator claims the Find shortcut before the global plugin search");
    panel.m_tree->setFocus();

    const auto previousControls = panel.m_visibleControls;
    const auto beforeAppearanceChange = controller.projectRevision();
    QMenu displayMenu; panel.populateMenu(displayMenu, true);
    for (int column = 1; column <= 3; ++column) {
        auto* action = displayMenu.findChild<QAction*>(QString("overview.showControl%1").arg(column));
        check(action && action->isCheckable() && action->isChecked() == previousControls[column - 1],
              "header menu checkmarks reflect visible controls");
        if (action) action->setChecked(false);
    }
    check(panel.m_tree->isColumnHidden(1) && panel.m_tree->isColumnHidden(2) && panel.m_tree->isColumnHidden(3) &&
          controller.projectRevision() == beforeAppearanceChange,
          "hiding all controls changes only the overview without editing tracks");
    {
        TrackOverviewPanel reopened(&controller);
        check(reopened.m_visibleControls == std::array<bool, 3>{false, false, false},
              "control visibility is restored in a newly opened panel");
    }
    panel.resize(200, 560); QApplication::processEvents();
    check(panel.m_tree->isColumnHidden(1) && panel.m_tree->isColumnHidden(2) &&
          panel.m_tree->isColumnHidden(3) && panel.m_tree->columnWidth(0) >= 190,
          "hidden controls return their column space to names in the wide panel");
    auto* showMute = displayMenu.findChild<QAction*>("overview.showControl1");
    if (showMute) showMute->setChecked(true);
    check(!panel.m_tree->isColumnHidden(1) && panel.m_tree->isColumnHidden(2) && panel.m_tree->isColumnHidden(3),
          "each display option restores only its own control");
    for (int column = 1; column <= 3; ++column)
        if (auto* action = displayMenu.findChild<QAction*>(QString("overview.showControl%1").arg(column)))
            action->setChecked(previousControls[column - 1]);

    panel.setSelectedTracks({q(lead)});
    QKeyEvent moveUp(QEvent::KeyPress, Qt::Key_Up, Qt::AltModifier);
    QApplication::sendEvent(panel.m_tree, &moveUp);
    check(project.indexOf(lead) < project.indexOf(bass), "Alt-Up reorders selected sibling tracks");
    controller.undo(); panel.refresh();

    // Real widget renders, with native item-view selection, keyboard focus and
    // the same theme tokens as the surrounding desktop panels.
    controller.setTracksEditLocked({vocals}, true);
    controller.setTrackMuted(snare, true); controller.setTrackSoloed(bass, true);
    panel.refresh(); panel.setSelectedTracks({q(kick), q(snare)}, q(snare));
    if (!screenshots.isEmpty()) {
        QDir().mkpath(screenshots);
        for (const auto& theme : ThemeManager::instance().presets()) {
            if (theme.dark != th().dark) {
                ThemeManager::instance().setThemeId(theme.id, false);
                break;
            }
        }
        for (int pass = 0; pass < 2; ++pass) {
            for (int width : {180, 200, 280}) {
                panel.resize(width, 560); panel.activateWindow(); panel.m_tree->setFocus(); QApplication::processEvents();
                check(panel.m_tree->columnWidth(0) >= 60 && panel.width() == width,
                      "each layout keeps a usable name column within its requested width");
                check(panel.grab().save(QDir(screenshots).filePath(
                    QString("tracks-%1-%2.png").arg(th().dark ? "dark" : "light").arg(width))), "theme screenshot saved");
            }
            panel.m_search->setFocus(); QApplication::processEvents();
            check(panel.grab().save(QDir(screenshots).filePath(
                QString("tracks-%1-inactive.png").arg(th().dark ? "dark" : "light"))), "inactive selection screenshot saved");
            ThemeManager::instance().setThemeId(previousTheme, false);
        }
        QMenu display; panel.populateMenu(display, true);
        display.popup(QPoint(0, 0)); QApplication::processEvents();
        check(display.grab().save(QDir(screenshots).filePath("tracks-menu.png")), "display menu screenshot saved");
        display.close();
    }
    {
        MainWindow window(false); window.populateDemo();
        window.resize(1440, 900); window.show(); QApplication::processEvents();
        auto* overview = window.findChild<TrackOverviewPanel*>();
        auto* headers = window.findChild<TrackListWidget*>();
        auto* browser = window.findChild<FileBrowserPanel*>();
        auto* inspector = window.findChild<InspectorWidget*>();
        if (!check(overview && headers && browser && inspector, "panel is integrated into the workspace")) return false;
        check(overview->width() == qBound(180, QSettings().value("ui/trackOverviewWidth", 180).toInt(), 420),
              "workspace defaults to 180 px and clamps saved narrower widths");
        check(browser->mapTo(&window, QPoint()).x() < overview->mapTo(&window, QPoint()).x() &&
              overview->mapTo(&window, QPoint()).x() < inspector->mapTo(&window, QPoint()).x(),
              "overview sits between file browser and inspector");
        QStringList ids;
        for (const auto& track : overview->m_controller->project().tracks) {
            if (daw::isFolder(track)) continue;
            ids.push_back(QString::fromStdString(track.id)); if (ids.size() == 2) break;
        }
        overview->setSelectedTracks(ids); overview->publishSelection();
        check(headers->selectedTrackIds() == ids, "overview multi-selection reaches timeline headers");
        if (ids.size() == 2) {
            clickIn(*overview, ids.front().toStdString());
            clickIn(*overview, ids.back().toStdString(), Qt::ControlModifier);
            check(overview->selectedTrackIds() == ids && headers->selectedTrackIds() == ids,
                  "workspace selection echo preserves Ctrl-click selection");
            clickIn(*overview, ids.front().toStdString());
            clickIn(*overview, ids.back().toStdString(), Qt::ShiftModifier);
            check(overview->selectedTrackIds() == ids && headers->selectedTrackIds() == ids,
                  "workspace selection echo preserves the native Shift anchor");
            if (syntheticDrags) {
                QString destination;
                const auto* source = overview->m_controller->project().findTrack(ids.front().toStdString());
                for (const auto& track : overview->m_controller->project().tracks)
                    if (track.kind == daw::TrackKind::Folder && track.id != source->parentId) {
                        destination = QString::fromStdString(track.id); break;
                    }
                clickIn(*overview, ids.front().toStdString());
                check(!destination.isEmpty() && dragIn(*overview, ids.front(), destination, true) &&
                      overview->selectedTrackIds() == QStringList{ids.front()} &&
                      headers->selectedTrackIds() == QStringList{ids.front()},
                      "GPU pointer routing and workspace refresh keep only the moved track selected");
                clickIn(*overview, ids.back().toStdString());
                check(overview->selectedTrackIds() == QStringList{ids.back()} &&
                      headers->selectedTrackIds() == QStringList{ids.back()},
                      "the next workspace click replaces the selection after a drop");
            }
        }
        auto* toggle = window.findChild<QAction*>("view.toggleTrackOverview");
        if (check(toggle, "View menu exposes the panel command")) {
            toggle->trigger(); check(overview->isHidden(), "panel command hides the view");
            toggle->trigger(); check(!overview->isHidden(), "panel command restores the view");
        }
        if (!screenshots.isEmpty()) {
            if (!ids.isEmpty()) clickIn(*overview, ids.front().toStdString());
            window.activateWindow(); overview->m_tree->setFocus();
            QApplication::processEvents();
            auto* surface = window.findChild<ui::graphics::WorkspaceSurface*>();
            const auto frame = surface ? surface->grabImage() : window.grab().toImage();
            check(frame.save(QDir(screenshots).filePath("workspace.png")), "workspace screenshot saved");
            const QPoint origin = overview->mapTo(surface ? window.centralWidget() : &window, QPoint());
            const qreal dpr = frame.devicePixelRatio();
            check(frame.copy(QRect(qRound(origin.x() * dpr), qRound(origin.y() * dpr),
                                  qRound(overview->width() * dpr), qRound(overview->height() * dpr)))
                      .save(QDir(screenshots).filePath("workspace-tracks.png")), "GPU overview detail saved");
        }
    }
    std::fflush(stdout);
    return ok;
}
