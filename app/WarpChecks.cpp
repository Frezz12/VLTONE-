#include "MainWindow.hpp"
#include "MixerWidget.hpp"
#include "TimelineWidget.hpp"
#include "WarpEditorWidget.hpp"
#include "TransportBar.hpp"
#include "Controls.hpp"
#include "Recording/RecordingEngine.hpp"
#include <QApplication>
#include <QAbstractButton>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QLayout>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

bool MainWindow::checkWarpForTest() {
    bool ok = true;
    const auto check = [&](bool pass, const char* label) { std::fprintf(stderr, "%s %s\n", pass ? "PASS" : "FAIL", label); ok &= pass; };
    const auto original = m_controller.project();
    QTemporaryDir directory;
    audio::AudioBuffer audio(2, 96000);
    for (int i = 0; i < 96000; ++i) {
        const int age = i % 12000;
        const float value = age < 4000 ? float(.75 * std::exp(-age / 1000.) * std::cos(age * .028)) : 0;
        audio.getChannel(0)[i] = value; audio.getChannel(1)[i] = value;
    }
    const auto file = directory.filePath("drums.wav").toStdString();
    audio::AudioRecorder recorder; recorder.initialize(48000, 2); recorder.writeWAVFile(file, audio, 48000);
    daw::ProjectModel project; project.tempo = 120;
    daw::TrackModel track; track.id = daw::newUuid(); track.kind = daw::TrackKind::Audio; track.name = "Drums";
    daw::ClipModel first; first.id = daw::newUuid(); first.kind = daw::ClipKind::Audio;
    first.name = "Acoustic loop"; first.filePath = file; first.durationSeconds = 2; first.channels = 2;
    auto second = first; second.id = daw::newUuid(); second.name = "Second take"; second.startSeconds = 3;
    track.clips = {first, second}; project.tracks = {track};
    m_controller.restoreProject(project, "Warp panel test"); syncViews();
    const auto trackId = QString::fromStdString(track.id), clipId = QString::fromStdString(first.id);
    setMixerVisible(false);
    m_timeline->selectClips({{trackId, QString::fromStdString(second.id)}, {trackId, clipId}});
    bool choseWarp = false;
    QTimer::singleShot(0, this, [&] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            for (auto* action : menu->actions()) if (action->objectName() == "WarpAudioAction") {
                choseWarp = action->isEnabled(); menu->setActiveAction(action);
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QApplication::sendEvent(menu, &enter); return;
            }
            menu->close();
        }
    });
    const QPoint clicked(int((.5 - m_timeline->horizontalScrollForTest()) * m_timeline->pixelsPerSecondForTest()),
                         m_timeline->laneCentreForTest(0));
    QContextMenuEvent context(QContextMenuEvent::Mouse, clicked, m_timeline->mapToGlobal(clicked));
    QApplication::sendEvent(m_timeline, &context);
    check(choseWarp, "actual context menu exposes Warp for the clip under the pointer in a multiple selection");
    QApplication::processEvents();
    check(m_warpPanel->isVisible() && m_warpEditor->window() == this &&
          m_warpPanel->parentWidget() == m_arrangementHost &&
          m_warpEditor->clipId() == clipId && m_bottomPanel->isHidden() &&
          m_timeline->bottomInsetForTest() == m_arrangementHost->height() - m_warpPanel->y() &&
          m_warpPanel->geometry().bottom() + 1 == m_arrangementHost->height(),
          "Warp opens the selected clip in a panel attached to the bottom of the arrangement");
    selectTrackFromHeader(trackId);
    check(m_warpEditor->clipId() == clipId, "ordinary selection does not switch Warp clip");
    auto map = m_controller.audioClip(track.id, first.id)->warp;
    map.markers.insert(map.markers.begin() + 1, {daw::newUuid(), .5, 1.5, false});
    m_controller.setClipWarp(track.id, first.id, map); m_warpEditor->refresh();
    m_showMixerAction->trigger(); QApplication::processEvents();
    check(m_warpPanel->isHidden() && m_bottomPanel->isVisible() &&
          m_warpEditor->clipId() == clipId && !m_showWarpAction->isChecked() &&
          m_showMixerAction->isChecked(),
          "X switches to the mixer while retaining the Warp clip");
    check(m_bottomPanel->layout()->count() == 1 &&
          m_mixer->geometry().bottom() + 1 == m_bottomPanel->height() &&
          m_bottomPanel->geometry().bottom() + 1 == m_arrangementHost->height(),
          "mixer fills the bottom edge with no Detach / Dock footer");
    if (const auto path = qEnvironmentVariable("DAW_MIXER_DOCK_SCREENSHOT"); !path.isEmpty()) grab().save(path);
    m_showWarpAction->trigger(); QApplication::processEvents();
    check(m_warpPanel->isVisible() && m_bottomPanel->isHidden() &&
          !m_showMixerAction->isChecked() && m_showWarpAction->isChecked(),
          "W switches back to the bottom Warp panel without tabs or a separate window");

    const int savedWarpHeight = m_warpHeight;
    const int savedMixerHeight = m_mixerHeight;
    m_warpHeight = 280;
    layoutBottomPanels();
    auto* resize = qobject_cast<ui::ResizeHandle*>(m_warpHandle);
    check(resize && resize->onDragStart && resize->onDrag, "Warp has a top-edge resize handle");
    if (resize && resize->onDragStart && resize->onDrag) {
        resize->onDragStart(); resize->onDrag(-40);
        check(m_warpHeight == 320 && m_mixerHeight == savedMixerHeight &&
              m_warpPanel->geometry().bottom() + 1 == m_arrangementHost->height(),
              "Warp expands upward and retains its own height");
        resize->onDrag(40);
        check(m_warpHeight == 240 &&
              m_warpPanel->geometry().bottom() + 1 == m_arrangementHost->height(),
              "reversing the resize follows the pointer while the bottom stays fixed");
        resize->onDrag(m_warpDragStartHeight);
        check(m_warpPanel->isHidden() && !m_showWarpAction->isChecked() &&
              m_timeline->bottomInsetForTest() == 0,
              "dragging Warp to the bottom closes it and uncovers the timeline");
        toggleWarpEditor();
        check(m_warpPanel->isVisible() && m_warpHeight == 280,
              "Warp reopens at the height from before drag dismissal");
    }
    const QSize hostSize = m_arrangementHost->size();
    m_arrangementHost->resize(hostSize.width(), 80);
    layoutBottomPanels();
    check(m_warpPanel->y() >= 0 && m_warpHandle->y() >= 0 &&
          m_timeline->bottomInsetForTest() == m_arrangementHost->height() - m_warpPanel->y(),
          "short windows keep the Warp resize edge reachable and the timeline inset accurate");
    m_arrangementHost->resize(hostSize);
    m_warpHeight = savedWarpHeight;
    layoutBottomPanels();
    m_showWarpAction->trigger(); QApplication::processEvents();
    check(m_warpPanel->isHidden() && !m_showWarpAction->isChecked() &&
          m_timeline->bottomInsetForTest() == 0,
          "W closes Warp and synchronizes its menu command");
    check(m_showWarpAction->shortcut() == QKeySequence(Qt::Key_W) &&
          m_showMixerAction->shortcut() == QKeySequence(Qt::Key_X),
          "Warp and Mixer have separate W and X shortcuts");
    auto* warpButton = m_transport->findChild<QAbstractButton*>("HeaderWarpButton");
    check(warpButton != nullptr, "transport has a separate Warp button");
    // The offscreen popup test can leave no active top-level window. Restore
    // the workspace activation a real click on its toolbar supplies.
    QApplication::setActiveWindow(this);
    if (warpButton) warpButton->click();
    QApplication::processEvents();
    check(m_warpPanel->isVisible() && m_warpEditor->clipId() == clipId &&
          warpButton && warpButton->isChecked() && m_warpEditor->ownsEditingFocus(),
          "Warp button reopens the pinned clip and returns keyboard focus to its canvas");
    if (warpButton) warpButton->click();
    check(m_warpPanel->isHidden() && !m_showWarpAction->isChecked() &&
          warpButton && !warpButton->isChecked(), "Warp button also closes the bottom panel");
    openWarpEditor(trackId, QString::fromStdString(second.id));
    check(m_warpEditor->clipId() == QString::fromStdString(second.id) && m_controller.audioClip(track.id, first.id)->warp == map,
          "opening another clip leaves the first Warp map intact");
    openWarpEditor(trackId, clipId);
    QEventLoop wait; QTimer::singleShot(250, &wait, &QEventLoop::quit); wait.exec();
    if (const auto path = qEnvironmentVariable("DAW_WARP_SCREENSHOT"); !path.isEmpty()) grab().save(path);
    m_controller.removeClip(track.id, first.id); syncViews();
    check(!m_controller.audioClip(track.id, first.id), "deleting the edited clip refreshes safely");
    m_warpEditor->clearClip(); m_controller.restoreProject(original, "Restore after Warp test"); syncViews(); setMixerVisible(true);
    return ok;
}
