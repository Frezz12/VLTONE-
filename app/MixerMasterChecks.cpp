#include "MixerWidget.hpp"
#include "MixerPreferences.hpp"
#include "Controls.hpp"
#include "EngineController.hpp"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSettings>
#include <QToolButton>
#include <QWheelEvent>
#include <cstdio>

bool MixerWidget::checkMasterDockForTest() {
    auto& preferences = ui::MixerPreferences::instance();
    const bool originalVisible = preferences.masterVisible();
    const int originalWidth = preferences.channelWidth();
    const auto restore = qScopeGuard([&] {
        preferences.setMasterVisible(originalVisible);
        preferences.setChannelWidth(originalWidth);
    });
    preferences.setMasterVisible(true); preferences.setChannelWidth(100);
    daw::EngineController controller{};
    if (!controller.initialize(48000, 256, false)) return false;
    auto& project = const_cast<daw::ProjectModel&>(controller.project());
    project.tracks.clear(); project.invalidateTrackIndex();
    for (int i = 0; i < 12; ++i) {
        daw::TrackModel track;
        track.id = "master-dock-" + std::to_string(i);
        track.name = "Track " + std::to_string(i + 1);
        track.kind = i % 2 ? daw::TrackKind::Instrument : daw::TrackKind::Audio;
        project.tracks.push_back(std::move(track));
    }
    MixerWidget mixer(&controller);
    mixer.setAttribute(Qt::WA_DontShowOnScreen); mixer.resize(780, 640); mixer.show();
    const auto settle = [&] {
        for (int i = 0; i < 24; ++i) { QApplication::processEvents(); mixer.syncVisibleStrips(); }
    };
    const auto master = [&]() -> ChannelStrip* {
        for (auto* strip : mixer.m_strips) if (strip->isMaster()) return strip;
        return nullptr;
    };
    bool ok = true;
    const auto check = [&](bool value, const char* message) {
        std::fprintf(stderr, "%s master dock: %s\n", value ? "PASS" : "FAIL", message);
        ok &= value;
    };
    const auto shots = qEnvironmentVariable("DAW_MASTER_DOCK_SCREENSHOTS");
    const auto shot = [&](const char* name) {
        if (!shots.isEmpty()) check(mixer.grab().save(shots + QLatin1String(name) + ".png"), "screenshot saved");
    };
    settle();
    auto* initialMaster = master();
    if (!initialMaster) return false;
    const auto aligned = [&] {
        const auto* mainScroll = mixer.m_scroll->verticalScrollBar();
        const auto* masterScroll = mixer.m_masterScroll->verticalScrollBar();
        if (mainScroll->maximum() != masterScroll->maximum() ||
            mainScroll->value() != masterScroll->value()) return false;
        for (auto* strip : mixer.m_strips) {
            if (strip->isMaster()) continue;
            if (strip->height() != master()->height()) return false;
            for (const char* name : {"ChannelHeader", "ChannelProcessingRow", "ChannelLevelWell"}) {
                const auto* row = strip->findChild<QWidget*>(QLatin1String(name));
                const auto* masterRow = master()->findChild<QWidget*>(QLatin1String(name));
                if (!row || !masterRow || row->height() != masterRow->height() ||
                    row->mapTo(&mixer, QPoint()).y() != masterRow->mapTo(&mixer, QPoint()).y()) return false;
            }
            const auto* trackFader = strip->findChild<ui::FaderWidget*>();
            const auto* masterFader = master()->findChild<ui::FaderWidget*>();
            if (trackFader->height() != masterFader->height() ||
                trackFader->mapTo(&mixer, QPoint()).y() != masterFader->mapTo(&mixer, QPoint()).y()) return false;
        }
        return true;
    };
    check(aligned(), "Master shares the channel row and fader geometry");
    check(!mixer.findChild<QWidget*>(QStringLiteral("MasterFoldHandle")) &&
          mixer.m_masterEdge->parentWidget() == mixer.m_masterDock &&
          mixer.m_masterDock->x() == mixer.m_scroll->geometry().right() + 1,
          "Master has no separate handle or extra gutter");
    auto* fader = initialMaster->findChild<ui::FaderWidget*>();
    const auto undoDepth = controller.undoDepth();
    const auto gain = controller.masterVolume();
    const auto addEffects = [](auto& inserts, int count) {
        inserts.clear();
        for (int i = 0; i < count; ++i) {
            daw::InsertModel insert;
            insert.id = "dock-fx-" + std::to_string(i);
            insert.name = "Effect " + std::to_string(i + 1);
            inserts.push_back(std::move(insert));
        }
    };
    addEffects(project.tracks.back().inserts, 24);
    mixer.rebuild(); settle();
    check(master() == initialMaster && master()->insertSlotCount() == 2 && aligned(),
          "24 inserts on another track keep Master aligned without recreating it or adding slots");
    check(mixer.m_scroll->verticalScrollBar()->maximum() > 0 &&
          mixer.m_masterScroll->verticalScrollBar()->maximum() == mixer.m_scroll->verticalScrollBar()->maximum(),
          "the whole channel row shares one vertical scroll range");
    shot("-aligned");
    mixer.m_scroll->verticalScrollBar()->setValue(mixer.m_scroll->verticalScrollBar()->maximum());
    settle();
    check(aligned(), "scrolling tracks moves the Master fader by the same distance");
    shot("-scrolled");
    const int masterX = master()->mapTo(&mixer, QPoint()).x();
    mixer.m_scroll->horizontalScrollBar()->setValue(mixer.m_scroll->horizontalScrollBar()->maximum());
    settle();
    check(aligned() && master()->mapTo(&mixer, QPoint()).x() == masterX,
          "horizontal scrolling keeps Master pinned on the right and vertically aligned");
    mixer.m_scroll->verticalScrollBar()->setValue(0); settle();

    const int openViewportWidth = mixer.m_scroll->viewport()->width();
    mixer.m_masterToggle->click(); settle();
    collab::SemanticPoint presence;
    presence.surface = {collab::SurfaceKind::Mixer, QStringLiteral("main"), {}};
    presence.targetId = "master_strip"; presence.laneFraction = .5;
    check(mixer.m_masterDock->width() == 0 && !mixer.m_masterColumn->isVisible() &&
          !mixer.m_masterEdge->isVisible() && !mixer.m_masterToggle->isChecked() &&
          mixer.m_scroll->viewport()->width() > openViewportWidth &&
          !mixer.collaborationPositionFor(presence), "button hides Master, releases space and removes hidden presence");
    shot("-hidden");
    MixerWidget peer(&controller);
    check(!peer.m_masterVisible && !QSettings().value(ui::MixerPreferences::kMasterVisibleSetting).toBool(),
          "hidden state persists and applies to newly opened consoles");
    // Keyboard is a complete alternative to dragging the channel edge.
    QKeyEvent keyDown(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent keyUp(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(mixer.m_masterToggle, &keyDown);
    QApplication::sendEvent(mixer.m_masterToggle, &keyUp); settle();
    check(mixer.m_masterVisible && peer.m_masterVisible && master() == initialMaster &&
          mixer.collaborationPositionFor(presence).has_value(), "Space restores the same strip and synchronizes other consoles");

    auto* edge = mixer.m_masterEdge;
    QPoint origin;
    const auto mouse = [&](QEvent::Type type, int delta) {
        const QPoint global = origin + QPoint(delta, 0);
        QMouseEvent event(type, QPointF(edge->mapFromGlobal(global)), QPointF(global),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(edge, &event);
        QApplication::processEvents();
    };
    const auto press = [&] {
        const QPoint at = mixer.m_masterDock->mapTo(&mixer, QPoint(1, mixer.m_masterDock->height() / 2));
        check(mixer.childAt(at) == edge, "the channel's left edge receives the grab");
        origin = mixer.mapToGlobal(at);
        mouse(QEvent::MouseButtonPress, 0);
    };
    const int fullWidth = mixer.masterExpandedWidth();
    press(); mouse(QEvent::MouseButtonRelease, 0); settle();
    check(mixer.m_masterVisible && mixer.m_masterDock->width() == fullWidth,
          "a click on the edge without dragging leaves Master open");
    check(!edge->rect().contains(edge->mapFromGlobal(fader->mapToGlobal(fader->rect().center()))),
          "the edge does not intercept the Master fader");
    press(); mouse(QEvent::MouseMove, 35);
    check(mixer.m_masterDock->width() == fullWidth - 35 && master()->width() == 100,
          "drag clips the strip one-to-one without shrinking its controls");
    mouse(QEvent::MouseMove, 15);
    check(mixer.m_masterDock->width() == fullWidth - 15, "direction reversal immediately follows the pointer");
    mouse(QEvent::MouseButtonRelease, 15); settle();
    check(mixer.m_masterDock->width() == fullWidth, "a short drag settles open");
    press(); mouse(QEvent::MouseMove, fullWidth + 20); mouse(QEvent::MouseButtonRelease, fullWidth + 20); settle();
    check(!mixer.m_masterVisible && mixer.m_masterDock->width() == 0 && !edge->isVisible(),
          "dragging the left edge right collapses Master without leaving a handle");
    mixer.m_masterToggle->click(); settle();
    check(mixer.m_masterVisible && mixer.m_masterDock->width() == fullWidth && edge->isVisible(),
          "the header button restores Master and its edge");
    press(); mouse(QEvent::MouseMove, 35);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(edge, &escape);
    mouse(QEvent::MouseButtonRelease, 35); settle();
    check(mixer.m_masterVisible && mixer.m_masterDock->width() == fullWidth && preferences.masterVisible(),
          "Escape cancels a drag without persisting the preview");

    for (int width : {75, 180, 100}) {
        preferences.setChannelWidth(width); settle();
        mixer.m_masterToggle->click(); mixer.resize(620, 500); mixer.rebuild(); settle();
        check(mixer.m_masterDock->width() == 0, "hidden state survives density changes, resize and rebuild");
        mixer.m_masterToggle->click(); settle();
        check(master()->width() == width && mixer.m_masterDock->width() == mixer.masterExpandedWidth() && aligned(),
              "restore adapts to the configured channel width and shared row geometry");
    }
    mixer.resize(780, 640); settle();
    for (auto& track : project.tracks) track.inserts.clear();
    mixer.rebuild(); settle();
    addEffects(project.masterInserts, 22);
    mixer.rebuild(); settle();
    check(mixer.m_insertSlotCount == 2 && master()->insertSlotCount() == 23 &&
          mixer.m_masterScroll->verticalScrollBar()->maximum() > 0 && aligned(),
          "a long Master chain aligns the whole row while preserving each channel's slot count");
    mixer.m_masterScroll->verticalScrollBar()->setValue(mixer.m_masterScroll->verticalScrollBar()->maximum()); settle();
    check(aligned() && mixer.m_scroll->verticalScrollBar()->value() > 0 &&
          master()->width() <= mixer.m_masterScroll->viewport()->width(),
          "scrolling Master moves the whole row without clipping controls");
    shot("-master-scroll");
    const int beforeWheel = mixer.m_scroll->verticalScrollBar()->value();
    auto* viewport = mixer.m_masterScroll->viewport();
    const QPointF at(viewport->rect().center());
    QWheelEvent wheel(at, viewport->mapToGlobal(at), QPoint(0, 41), {},
                      Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(viewport, &wheel); settle();
    check(aligned() && mixer.m_scroll->verticalScrollBar()->value() < beforeWheel,
          "a precise wheel gesture over Master scrolls the shared row");
    project.masterInserts.clear();
    project.tracks.resize(3);
    project.invalidateTrackIndex();
    mixer.rebuild(); settle();
    check(mixer.m_scroll->horizontalScrollBar()->maximum() == 0 && aligned() &&
          master()->mapTo(&mixer, QPoint()).x() == masterX,
          "a short channel row keeps Master aligned at the right edge without a horizontal scrollbar");
    shot("-short-row");
    check(controller.undoDepth() == undoDepth && controller.masterVolume() == gain,
          "folding, dragging and preferences leave audio level and Undo unchanged");
    return ok;
}
