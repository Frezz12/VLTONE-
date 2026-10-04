#include "TimelineWidget.hpp"
#include "TrackListWidget.hpp"
#include "Controls.hpp"
#include "CompLayout.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "Theme.hpp"
#include "UiFrameClock.hpp"
#include "graphics/WorkspaceSurface.hpp"
#include "graphics/GraphicsPreferences.hpp"
#include <QApplication>
#include <QContextMenuEvent>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <cmath>
#include <memory>

bool TimelineWidget::checkTrackPresentationForTest() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool ok = true;
    const auto check = [&](bool value, const char* name) {
        std::printf("%s  %s\n", value ? "PASS" : "FAIL", name);
        ok &= value;
    };
    const auto settle = [](int ms = 80) {
        QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
    };
    ThemeManager::instance().apply();
    daw::EngineController controller;
    controller.initialize(48000, 512, false);
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    project.tracks.clear();
    project.invalidateTrackIndex();
    QTemporaryDir files;
    audio::AudioBuffer source(1, 48000 * 8);
    for (unsigned i = 0; i < source.numFrames(); ++i)
        source.getChannel(0)[i] = float(.55 * std::sin(i * .034) *
            (.25 + .75 * std::exp(-std::fmod(i / 48000.0, .5) * 9)));
    const auto path = (files.path() + "/preview.wav").toStdString();
    audio::AudioRecorder writer;
    if (!writer.writeWAVFile(path, source, 48000).isOk()) return false;
    controller.waveforms().peaks(path);
    const auto add = [&](const std::string& id, const std::string& name, daw::TrackKind kind,
                         uint32_t color, const std::string& parent = {}) {
        daw::TrackModel track;
        track.id = id; track.name = name; track.kind = kind; track.color = color;
        track.parentId = parent; track.expanded = false;
        project.tracks.push_back(std::move(track));
        project.invalidateTrackIndex();
    };
    const auto clips = [&](const std::string& id, daw::ClipKind kind, double duration, int count) {
        auto* track = project.findTrack(id);
        for (int i = 0; i < count; ++i) {
            daw::ClipModel clip;
            clip.id = id + "-" + std::to_string(i);
            clip.name = track->name; clip.kind = kind;
            clip.startSeconds = .5 + i * (duration + .18);
            clip.durationSeconds = duration;
            if (kind == daw::ClipKind::Audio) { clip.filePath = path; clip.channels = 1; }
            else if (kind == daw::ClipKind::Midi) for (int n = 0; n < 12; ++n) {
                daw::NoteModel note;
                note.id = clip.id + "-note-" + std::to_string(n);
                note.pitch = 48 + (n % 5) * 3; note.startBeats = n * duration / 6;
                note.lengthBeats = duration / 9;
                clip.notes.push_back(note);
            }
            track->clips.push_back(clip);
        }
    };
    add("drums", "Drums", daw::TrackKind::Folder, 0xC48B56);
    project.findTrack("drums")->summing = true;
    project.findTrack("drums")->height = 112;
    add("kick", "Kick", daw::TrackKind::Audio, 0xC48B56, "drums"); clips("kick", daw::ClipKind::Audio, .35, 18);
    add("snare", "Snare", daw::TrackKind::Audio, 0xDCB169, "drums"); clips("snare", daw::ClipKind::Audio, .6, 11);
    add("hats", "Hi-hats", daw::TrackKind::Midi, 0xB6AA63, "drums"); clips("hats", daw::ClipKind::Midi, 2, 4);
    add("percussion", "Percussion", daw::TrackKind::Folder, 0x88B482, "drums");
    add("shaker", "Shaker", daw::TrackKind::Midi, 0x88B482, "percussion"); clips("shaker", daw::ClipKind::Midi, 1.2, 6);
    add("rim", "Rim", daw::TrackKind::Midi, 0x65AAA2, "percussion"); clips("rim", daw::ClipKind::Midi, .7, 9);
    add("bass", "Sub bass", daw::TrackKind::Audio, 0x638FCA); clips("bass", daw::ClipKind::Audio, 2.8, 3);
    project.findTrack("bass")->height = 96;
    add("lead", "Glass lead", daw::TrackKind::Instrument, 0x9C87C7); clips("lead", daw::ClipKind::Midi, 3.8, 2);
    add("vocal", "Vocal textures", daw::TrackKind::Audio, 0xC77A91); clips("vocal", daw::ClipKind::Audio, 3, 3);
    add("chords", "Chords", daw::TrackKind::Pattern, 0x829BCE); clips("chords", daw::ClipKind::Pattern, 3.8, 2);
    add("pad", "Pad", daw::TrackKind::Instrument, 0x829BCE, "chords"); clips("pad", daw::ClipKind::Midi, 3.8, 2);
    for (std::size_t i = 0; i < 2; ++i) project.findTrack("pad")->clips[i].patternClipId = "chords-" + std::to_string(i);
    add("envelope", "Volume automation", daw::TrackKind::Automation, 0x63B5B0);
    clips("envelope", daw::ClipKind::Automation, 3.8, 2);
    project.findTrack("envelope")->height = 24;
    for (auto& clip : project.findTrack("envelope")->clips) {
        clip.color = 0x63B5B0;
        clip.automation.points = {{0, .1}, {2, .85}, {4, .85}, {7.6, .25}};
    }
    add("empty", "Empty folder", daw::TrackKind::Folder, 0x828C98);
    project.invalidateStructure();

    QWidget host;
    auto* layout = new QHBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);
    TrackListWidget tracks(&controller, &host);
    TimelineWidget timeline(&controller, &host);
    timeline.m_pixelsPerSecond = 90;
    layout->addWidget(&tracks); layout->addWidget(&timeline, 1);
    QObject::connect(&tracks, &TrackListWidget::trackHeightChanged, &timeline, [&] {
        timeline.clampVerticalScroll(); timeline.update();
    });
    QObject::connect(&tracks, &TrackListWidget::orderChanged, &timeline, [&] {
        tracks.rebuild(); timeline.update();
    });
    QObject::connect(&timeline, &TimelineWidget::verticalScrollChanged, &tracks,
                     &TrackListWidget::setVerticalScroll);
    tracks.rebuild(); tracks.setSelectedTrack("lead"); timeline.setSelectedTrack("lead");
    host.resize(1260, 640);
    std::unique_ptr<ui::graphics::WorkspaceSurface> surface;
    if (ui::graphics::gpuWorkspaceEnabled()) {
        surface = std::make_unique<ui::graphics::WorkspaceSurface>(&host);
        QObject::connect(surface.get(), &ui::graphics::WorkspaceSurface::failed,
                         &host, [&](const QString& reason) {
            std::fprintf(stderr, "GPU presentation failed: %s\n", qPrintable(reason)); ok = false;
        });
    }
    // Create the native child before showing its parent, just like the actual
    // workspace and other native UI checks. A late container can stay hidden.
    host.show(); host.activateWindow();
    if (surface) surface->quickWindow()->requestActivate();
    settle(220);
    if (surface)
        check(surface->quickWindow()->isExposed(), "track presentation uses an exposed GPU window");
    const auto image = [&] {
        settle();
        return surface ? surface->quickWindow()->grabWindow() : host.grab().toImage();
    };
    const auto screenshot = [&](const QString& suffix) {
        const QString prefix = qEnvironmentVariable("VLT_TRACKS_SCREENSHOT");
        if (!prefix.isEmpty()) check(image().save(prefix + suffix + ".png"), "save track presentation screenshot");
    };
    const auto rowFor = [&](const QString& id) -> QWidget* {
        for (auto* row : tracks.findChildren<QWidget*>("TrackRow"))
            if (row->property("trackId").toString() == id)
                return row;
        return nullptr;
    };
    const auto doubleClick = [&](QWidget* target, const QPoint& local) {
        const QPointF global = target->mapToGlobal(local);
        const QPointF position = surface ? surface->quickWindow()->mapFromGlobal(global) : QPointF(local);
        for (auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                          QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
            QMouseEvent event(type, position, global, Qt::LeftButton,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                Qt::NoModifier);
            QApplication::sendEvent(surface ? static_cast<QObject*>(surface->quickWindow()) : target, &event);
        }
    };
    const auto doubleClickHeader = [&](const QString& id, bool onName = true) {
        auto* row = rowFor(id);
        if (!row) return false;
        if (onName) {
            auto* name = row->findChild<ui::InlineNameEdit*>();
            if (!name) return false;
            doubleClick(name, name->rect().center());
        } else {
            auto* band = row->findChild<QWidget*>("TrackRowBand");
            auto* icon = row->findChild<QWidget*>("TrackIcon");
            if (!band || !icon) return false;
            const QPoint gap(icon->geometry().right() + 3, band->height() / 2);
            if (band->childAt(gap)) return false;
            doubleClick(band, gap);
        }
        settle();
        return true;
    };
    const auto toggleSelectedFromMenu = [&] {
        QMenu menu;
        if (!tracks.populateSelectedTrackActionsMenu(menu)) return false;
        auto* action = menu.findChild<QAction*>("track.toggleCompact");
        if (!action) return false;
        action->trigger(); settle();
        return true;
    };
    const auto aligned = [&] {
        const auto& rows = timeline.visibleRows();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto r = tracks.rowRectForTrack(QString::fromStdString(project.tracks[rows[i].index].id));
            if (r.top() != timeline.laneTop(int(i)) || r.height() != timeline.laneHeightAt(int(i))) return false;
        }
        return true;
    };
    const auto controlsFit = [&] {
        for (auto* row : tracks.findChildren<QWidget*>("TrackRow")) {
            if (!row->isVisible()) continue;
            QList<QWidget*> controls;
            for (auto* button : row->findChildren<QAbstractButton*>()) controls.push_back(button);
            for (auto* fader : row->findChildren<ui::FaderWidget*>()) controls.push_back(fader);
            for (auto* pan : row->findChildren<ui::PanKnob*>()) controls.push_back(pan);
            for (auto* name : row->findChildren<QLineEdit*>()) controls.push_back(name);
            QList<QRect> rectangles;
            for (auto* control : controls) {
                if (!control->isVisibleTo(row)) continue;
                const QRect r(control->mapTo(row, QPoint{}), control->size());
                if (!row->rect().contains(r)) {
                    std::fprintf(stderr, "control outside %s: %s %d,%d %dx%d in %dx%d\n",
                        qPrintable(row->property("trackId").toString()), control->metaObject()->className(),
                        r.x(), r.y(), r.width(), r.height(), row->width(), row->height());
                    return false;
                }
                for (const auto& other : rectangles) if (r.intersects(other)) {
                    std::fprintf(stderr, "overlap in %s: %s %d,%d %dx%d with %d,%d %dx%d\n",
                        qPrintable(row->property("trackId").toString()), control->metaObject()->className(),
                        r.x(), r.y(), r.width(), r.height(), other.x(), other.y(), other.width(), other.height());
                    return false;
                }
                rectangles.push_back(r);
            }
        }
        return true;
    };
    check(aligned() && controlsFit(), "normal track controls fit and align with the timeline");
    const int curveLane = timeline.laneForTrackId("envelope");
    const auto curveBody = timeline.clipRect(curveLane, project.findTrack("envelope")->clips.front());
    PointHit curveHit;
    check(timeline.automationValueToY(curveBody, 1) < timeline.automationValueToY(curveBody, 0) &&
        !timeline.hitTestAutomationPoint(curveBody.center().toPoint(), curveHit),
        "compact automation retains the curve direction and hides breakpoint hit targets");
    screenshot("-normal");

    check(tracks.findChildren<QWidget*>("TrackCompactButton").isEmpty(),
          "track headers contain no dedicated minimize icon");
    check(doubleClickHeader("bass") && project.findTrack("bass")->height == 24 &&
          project.findTrack("lead")->height == 72 && aligned() &&
          rowFor("bass")->findChild<ui::InlineNameEdit*>()->isReadOnly(),
          "double-clicking the name minimizes just that track without starting rename");
    check(doubleClickHeader("bass", false) && project.findTrack("bass")->height == 96 && aligned(),
          "double-clicking free header space restores the previous height");
    auto* bassRow = rowFor("bass");
    for (QWidget* control : std::initializer_list<QWidget*>{bassRow->findChild<ui::FaderWidget*>(),
             bassRow->findChild<ui::PanKnob*>(), bassRow->findChild<ui::MsrButton*>()}) {
        doubleClick(control, control->rect().center()); settle();
        check(project.findTrack("bass")->height == 96,
              "double-clicking a channel control preserves the track height");
    }
    auto* name = bassRow->findChild<ui::InlineNameEdit*>();
    bool renameAvailable = false;
    QTimer::singleShot(0, &host, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) return;
        auto* action = menu->findChild<QAction*>("track.rename");
        renameAvailable = action != nullptr;
        menu->close();
        if (action) action->trigger();
    });
    const QPoint local = name->rect().center();
    const QPoint global = name->mapToGlobal(local);
    QContextMenuEvent context(QContextMenuEvent::Mouse,
        surface ? surface->quickWindow()->mapFromGlobal(global) : local, global);
    QApplication::sendEvent(surface ? static_cast<QObject*>(surface->quickWindow()) : name, &context);
    settle();
    check(renameAvailable && !name->isReadOnly(), "right-clicking a track name offers inline Rename");
    doubleClick(name, name->rect().center());
    check(!name->isReadOnly() && project.findTrack("bass")->height == 96,
          "double-clicking while renaming retains text editing and track height");
    name->setText("Renamed bass");
    QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(surface ? static_cast<QObject*>(surface->quickWindow()) : name, &commit);
    check(name->isReadOnly() && project.findTrack("bass")->name == "Renamed bass",
          "Return commits the inline name after the menu closes");
    controller.renameTrack("bass", "Sub bass"); tracks.syncTrackValues();
    tracks.setSelectedTracks({"bass", "lead"}, "bass");
    check(toggleSelectedFromMenu(), "track menu provides a keyboard-accessible minimize action");
    check(project.findTrack("bass")->height == 24 && project.findTrack("lead")->height == 24 && aligned(),
          "one action minimizes selected tracks to 24 px in both columns");
    tracks.setRecordState(true, {"bass"});
    for (int width : {220, 300, 500}) {
        tracks.setFixedWidth(width); settle();
        check(controlsFit(), "compact M/S and record controls stay inside narrow headers");
    }
    tracks.setFixedWidth(300); settle();
    screenshot("-compact");
    check(toggleSelectedFromMenu(), "track menu restores a group of minimized tracks");
    check(project.findTrack("bass")->height == 96 && project.findTrack("lead")->height == 72 && aligned(),
          "each selected track restores its own previous height");
    tracks.setRecordState(false, {});
    for (int width : {220, 300, 500}) for (double height : {24.0, 30.0, 44.0, 72.0, 150.0}) {
        controller.setTrackHeight("bass", height);
        tracks.setFixedWidth(width); tracks.syncRowHeights(); timeline.update(); settle(10);
        check(controlsFit() && aligned(), "continuous row resizing keeps control geometry and lane geometry aligned");
    }
    tracks.setFixedWidth(300); controller.setTrackHeight("bass", 96);
    tracks.syncRowHeights(); timeline.update(); settle();

    const int folderLane = timeline.laneForTrackId("drums");
    check(folderLane >= 0 && timeline.laneForTrackId("shaker") < 0,
          "nested descendants are hidden while their folder overview remains visible");
    const QImage before = image();
    check(timeline.m_folderPreviewRows["drums"].size() == 6 && timeline.m_folderPreviewRows["drums"].back().depth == 1,
          "folder preview includes ordered nested descendants and hierarchy depth");
    const QPoint inside(timeline.secondsToX(1.0), timeline.laneTop(folderLane) + 10);
    ClipHit hit;
    check(!timeline.hitTestClip(inside, hit), "folder overview cannot accidentally edit hidden child clips");
    timeline.m_staticDirty = {};
    controller.setTrackMuted("kick", true);
    timeline.invalidateTrack("kick");
    check(timeline.m_staticDirty.contains(QPoint(20, timeline.laneTop(folderLane) + 10)),
          "editing a hidden child invalidates its visible folder preview");
    const QImage after = image();
    const double scale = double(before.width()) / host.width();
    const QRect folderPixels(int(tracks.width() * scale), int(timeline.laneTop(folderLane) * scale),
        int(timeline.width() * scale), int(timeline.laneHeightAt(folderLane) * scale));
    check(!before.isNull() && before.copy(folderPixels) != after.copy(folderPixels),
          "retained folder pixels refresh after a hidden child changes");
    controller.setTrackMuted("kick", false); timeline.invalidateTrack("kick");

    tracks.setSelectedTrack("drums");
    check(doubleClickHeader("drums"), "folder header supports the same double-click size gesture");
    check(aligned() && controlsFit() && timeline.laneHeightAt(folderLane) == 24,
          "a collapsed folder preview also fits a 24 px strip");
    screenshot("-folder-strip");
    doubleClickHeader("drums");
    controller.setFolderExpanded("drums", true); tracks.rebuild(); timeline.update(); settle();
    check(timeline.laneForTrackId("kick") >= 0 && aligned() && controlsFit(),
          "folder disclosure restores child lanes without changing the saved folder height");
    controller.setFolderExpanded("percussion", true); tracks.rebuild(); timeline.update(); settle();
    check(timeline.laneForTrackId("shaker") >= 0 && aligned() && controlsFit(),
          "nested folder disclosure preserves alignment and controls");
    screenshot("-expanded");
    controller.setFolderExpanded("drums", false); tracks.rebuild(); timeline.update(); settle();
    // Exercise real header input, through the native Quick window when used.
    const auto order = [&] {
        std::vector<std::string> ids;
        for (const auto& track : project.tracks) ids.push_back(track.id);
        return ids;
    };
    const auto originalOrder = order();
    const auto pointer = [&](QWidget* target, QEvent::Type type,
                             const QPoint& global, Qt::KeyboardModifiers mods) {
        const QPointF position = surface
            ? surface->quickWindow()->mapFromGlobal(global)
            : target->mapFromGlobal(global);
        QMouseEvent event(type, position, QPointF(global),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
            mods);
        QApplication::sendEvent(surface
            ? static_cast<QObject*>(surface->quickWindow()) : target, &event);
        settle(35);
    };
    const auto clickName = [&](const QString& id, Qt::KeyboardModifiers mods) {
        auto* row = rowFor(id);
        auto* target = row ? row->findChild<ui::InlineNameEdit*>() : nullptr;
        if (!target) return false;
        const QPoint global = target->mapToGlobal(target->rect().center());
        pointer(target, QEvent::MouseButtonPress, global, mods);
        pointer(target, QEvent::MouseButtonRelease, global, mods);
        return true;
    };
    const auto dragHeader = [&](const QString& id, const QPoint& drop,
                                bool onName = true) {
        auto* row = rowFor(id);
        auto* target = onName && row
            ? static_cast<QWidget*>(row->findChild<ui::InlineNameEdit*>()) : row;
        if (!target) return false;
        const QPoint from = target->mapToGlobal(
            onName ? target->rect().center() : QPoint(2, row->height() / 2));
        const QPoint to = tracks.mapToGlobal(drop);
        const auto selected = tracks.selectedTrackIds();
        const auto before = order();
        pointer(target, QEvent::MouseButtonPress, from, Qt::NoModifier);
        check(tracks.selectedTrackIds() == selected,
              "pressing a selected header preserves the entire group");
        pointer(target, QEvent::MouseMove, to, Qt::NoModifier);
        check(order() == before, "reorder is committed only on pointer release");
        pointer(target, QEvent::MouseButtonRelease, to, Qt::NoModifier);
        return true;
    };
    check(clickName("bass", Qt::NoModifier) &&
          clickName("lead", Qt::ControlModifier) &&
          tracks.selectedTrackIds() == QStringList{"bass", "lead"},
          "Ctrl-clicking names selects several tracks without losing the anchor");
    const auto moveDepth = controller.undoDepth();
    auto upward = originalOrder;
    std::erase(upward, std::string("bass")); std::erase(upward, std::string("lead"));
    upward.insert(upward.begin(), {"bass", "lead"});
    check(dragHeader("lead", QPoint(2, tracks.rowRectForTrack("drums").top() + 2)) &&
          order() == upward && controller.undoDepth() == moveDepth + 1 &&
          tracks.selectedTrackIds() == QStringList{"bass", "lead"} && aligned(),
          "dragging a selected name moves the group upward with one undo");
    controller.undo(); tracks.rebuild(); timeline.update(); settle();
    check(order() == originalOrder && aligned(), "one undo restores every moved lane");
    controller.redo(); tracks.rebuild(); timeline.update(); settle();
    check(order() == upward && aligned(), "redo restores every moved lane");
    controller.undo(); tracks.rebuild(); timeline.update(); settle();

    auto downward = originalOrder;
    std::erase(downward, std::string("bass")); std::erase(downward, std::string("lead"));
    downward.insert(downward.end(), {"bass", "lead"});
    check(dragHeader("bass", QPoint(2, tracks.rowRectForTrack("empty").bottom() - 2), false) &&
          order() == downward && tracks.selectedTrackId() == "bass" &&
          tracks.selectedTrackIds() == QStringList{"bass", "lead"} && aligned(),
          "dragging another selected row downward keeps the group order");
    controller.undo(); tracks.rebuild(); timeline.update(); settle();
    check(order() == originalOrder && controller.undoDepth() == moveDepth,
          "downward group undo restores both rows in one action");
    check(clickName("lead", Qt::NoModifier) &&
          tracks.selectedTrackIds() == QStringList{"lead"},
          "a plain click without a drag still selects only one track");

    // Expanded takes must not defeat minimizing the parent track.
    auto* bass = project.findTrack("bass");
    bass->clips.front().expanded = true;
    bass->clips.front().takes.emplace_back();
    controller.setTrackHeight("bass", 24); tracks.syncRowHeights(); timeline.update(); settle();
    check(ui::laneHeightForTrack(*bass) == 24 && aligned(), "minimizing a track hides its take rows without losing take disclosure");
    controller.setTrackHeight("bass", 96); tracks.syncRowHeights(); timeline.update(); settle();
    check(ui::laneHeightForTrack(*bass) > 96 && aligned(), "restoring a track restores its open take rows");
    surface.reset();
    return ok;
}
