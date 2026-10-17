#include "MainWindow.hpp"
#include "MixerWidget.hpp"
#include "TimelineWidget.hpp"
#include "WarpEditorWidget.hpp"
#include "SampleEditorWindow.hpp"
#include <QTabWidget>
#include <QToolButton>
#include <QDir>
#include <QFileInfo>
#include "Theme.hpp"
#include "TransportBar.hpp"
#include "Controls.hpp"
#include "Recording/RecordingEngine.hpp"
#include <QApplication>
#include <QAbstractButton>
#include <QContextMenuEvent>
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>
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
    daw::TrackModel track; track.id = daw::newUuid(); track.kind = daw::TrackKind::Audio; track.name = "Drums"; track.color = 0x417dbe;
    daw::ClipModel first; first.id = daw::newUuid(); first.kind = daw::ClipKind::Audio;
    first.name = "Acoustic loop"; first.filePath = file; first.durationSeconds = 2; first.channels = 2;
    auto second = first; second.id = daw::newUuid(); second.name = "Second take"; second.startSeconds = 3;
    track.clips = {first, second}; project.tracks = {track};
    m_controller.restoreProject(project, "Warp panel test"); syncViews();
    const auto trackId = QString::fromStdString(track.id), clipId = QString::fromStdString(first.id);
    setMixerVisible(false);
    const QString key = trackId + '/' + clipId;
    const auto unopenedDepth = m_controller.undoDepth();
    openSampleEditor(trackId, clipId);
    auto* sampleWindow = m_sampleEditors.value(key);
    auto* tabs = sampleWindow->findChild<QTabWidget*>("SamplerTabs");
    check(tabs && tabs->count() == 3 && tabs->currentIndex() == 0 &&
          m_controller.audioClip(track.id, first.id)->warp.empty() && m_controller.undoDepth() == unopenedDepth,
          "Sampler hosts three pages without initializing Warp or changing history");
    m_timeline->selectClips({{trackId, QString::fromStdString(second.id)}, {trackId, clipId}});
    bool choseWarp = false;
    QTimer::singleShot(0, this, [&] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            for (auto* action : menu->actions()) if (action->objectName() == "WarpAudioAction") {
                choseWarp = action->isEnabled() && action->text() == TimelineWidget::tr("Warp Editor"); menu->setActiveAction(action);
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
    check(m_warpEditor && tabs->currentWidget() == m_warpEditor &&
          sampleWindow->isAncestorOf(m_warpEditor) && m_sampleEditors.value(key) == sampleWindow &&
          m_warpEditor->clipId() == clipId && m_timeline->bottomInsetForTest() == 0 &&
          !findChild<QWidget*>("WarpBottomPanel"),
          "context-menu Warp reuses the sampler window and leaves the arrangement uncovered");
    selectTrackFromHeader(trackId);
    check(m_warpEditor->clipId() == clipId, "ordinary selection does not switch Warp clip");
    auto map = m_controller.audioClip(track.id, first.id)->warp;
    map.markers.insert(map.markers.begin() + 1, {daw::newUuid(), .5, 1.5, false});
    m_controller.setClipWarp(track.id, first.id, map); m_warpEditor->refresh();
    m_showMixerAction->trigger(); QApplication::processEvents();
    const int mixerInset = m_timeline->bottomInsetForTest();
    check(m_bottomPanel->isVisible() && m_warpEditor->isVisible() && tabs->currentWidget() == m_warpEditor && mixerInset > 0,
          "mixer and the sampler Warp tab stay open independently");
    m_timeline->selectClips({{trackId, clipId}});
    const auto reopenDepth = m_controller.undoDepth();
    m_showWarpAction->trigger(); QApplication::processEvents();
    check(m_sampleEditors.value(key) == sampleWindow && tabs->currentWidget() == m_warpEditor &&
          m_bottomPanel->isVisible() && m_timeline->bottomInsetForTest() == mixerInset &&
          m_controller.undoDepth() == reopenDepth,
          "W focuses the existing Warp tab without hiding Mixer or creating history");
    check(m_showWarpAction->shortcut() == QKeySequence(Qt::Key_W) &&
          m_showMixerAction->shortcut() == QKeySequence(Qt::Key_X) && !m_showWarpAction->isCheckable(),
          "Warp remains an editor command with its W shortcut");
    auto* warpButton = m_transport->findChild<QAbstractButton*>("HeaderWarpButton");
    openSampleEditor(trackId, clipId);
    check(tabs->currentIndex() == 0, "opening Sampler returns to its normal page in the same window");
    QApplication::setActiveWindow(this);
    if (warpButton) warpButton->click();
    QApplication::processEvents();
    check(warpButton && !warpButton->isCheckable() && tabs->currentWidget() == m_warpEditor &&
          m_warpEditor->ownsEditingFocus(), "transport Warp command opens the tab and focuses its canvas");
    m_warpEditor->previewAlignment();
    check(m_controller.warpPreviewActive(), "embedded Warp can audition timing");
    tabs->setCurrentIndex(1); QApplication::processEvents();
    check(!m_controller.warpPreviewActive() && m_controller.audioClip(track.id, first.id)->warp == map,
          "switching to the audio Editor cancels uncommitted Warp audition");
    openWarpEditor(trackId, QString::fromStdString(second.id));
    check(m_warpEditor->clipId() == QString::fromStdString(second.id) && m_sampleEditors.size() == 2 &&
          m_controller.audioClip(track.id, first.id)->warp == map,
          "each clip has its own sampler window and Warp map");
    openWarpEditor(trackId, clipId);
    QEventLoop wait; QTimer::singleShot(500, &wait, &QEventLoop::quit); wait.exec();
    if (const auto path = qEnvironmentVariable("DAW_WARP_SCREENSHOT"); !path.isEmpty()) {
        const QString previous = ThemeManager::instance().theme().id;
        for (const auto& theme : ThemeManager::instance().presets()) {
            ThemeManager::instance().setThemeId(theme.id, false); QApplication::processEvents();
            sampleWindow->grab().save(QFileInfo(path).absolutePath() + "/sampler-warp-" + theme.id + ".png");
        }
        ThemeManager::instance().setThemeId(previous, false);
        sampleWindow->grab().save(path);
    }
    m_controller.removeClip(track.id, first.id); syncViews(); closeOrphanedPluginEditors(); QApplication::processEvents();
    check(!m_sampleEditors.contains(key) && !m_controller.audioClip(track.id, first.id),
          "deleting the edited clip closes its unified window safely");

    // Import must leave timing untouched; only the explicit context-menu
    // command may analyze and fit the source to the project tempo.
    for (auto* window : m_sampleEditors) window->showSampler();
    audio::AudioBuffer loop(2, 576000);
    for (int i = 0; i < 576000; ++i) {
        const int beat = i / 24000, age = i % 24000;
        const float value = age < 1400 ? float(std::exp(-age / 185.) *
            (.48 * (age % 41 < 17 ? 1 : -1) + .52 * std::sin(age * .009817477)) *
            (beat % 4 == 0 ? 1 : .6)) : 0;
        loop.getChannel(0)[i] = value; loop.getChannel(1)[i] = value * .7f;
    }
    const auto loopFile = directory.filePath("steady-loop.wav");
    recorder.writeWAVFile(loopFile.toStdString(), loop, 48000);
    m_controller.setTempo(150); syncViews();
    QMimeData mime; mime.setUrls({QUrl::fromLocalFile(loopFile)});
    const QPoint dropAt(int((1 - m_timeline->horizontalScrollForTest()) * m_timeline->pixelsPerSecondForTest()),
                        m_timeline->laneCentreForTest(0));
    QDragEnterEvent enter(dropAt, Qt::CopyAction, &mime, Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(m_timeline, &enter);
    QDropEvent drop(QPointF(dropAt), Qt::CopyAction, &mime, Qt::LeftButton, Qt::AltModifier);
    QTimer::singleShot(0, qApp, [] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            menu->setActiveAction(menu->actions().front());
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(menu, &enter);
        }
    });
    QApplication::sendEvent(m_timeline, &drop);
    const auto* importedTrack = m_controller.project().findTrack(track.id);
    check(drop.isAccepted() && importedTrack && importedTrack->clips.size() == 2,
          "dropping an audio file imports it through background preparation");
    if (importedTrack && importedTrack->clips.size() == 2) {
        const auto imported = importedTrack->clips.back();
        check(imported.warp.empty() && imported.musicalAnalysis.empty() &&
              std::abs(imported.durationSeconds - 12) < .001 && m_controller.tempo() == 150,
              "a dropped 120 BPM loop keeps its original duration in a 150 BPM project");
        const auto importedId = QString::fromStdString(imported.id);
        const auto sibling = *m_controller.audioClip(track.id, second.id);
        m_timeline->selectClips({{trackId, importedId}, {trackId, QString::fromStdString(second.id)}});
        const auto beforeAutoWarp = m_controller.undoDepth();
        bool choseAutoWarp = false;
        QTimer::singleShot(0, this, [&] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (auto* action : menu->actions()) if (action->objectName() == "AutoWarpAudioAction") {
                    choseAutoWarp = action->isEnabled() && action->text() == TimelineWidget::tr("Auto Warp");
                    menu->setActiveAction(action);
                    if (const auto path = qEnvironmentVariable("DAW_AUTO_WARP_SCREENSHOT"); !path.isEmpty())
                        menu->grab().save(path);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(menu, &enter); return;
                }
                menu->close();
            }
        });
        const QPoint autoWarpAt(int((imported.startSeconds + .4 - m_timeline->horizontalScrollForTest()) *
                                     m_timeline->pixelsPerSecondForTest()), m_timeline->laneCentreForTest(0));
        QContextMenuEvent autoWarpMenu(QContextMenuEvent::Mouse, autoWarpAt, m_timeline->mapToGlobal(autoWarpAt));
        QApplication::sendEvent(m_timeline, &autoWarpMenu);
        const auto* fitted = m_controller.audioClip(track.id, imported.id);
        check(choseAutoWarp && fitted && fitted->warp.enabled &&
              std::abs(fitted->durationSeconds - 9.6) < .001 &&
              std::abs(fitted->warp.baselineDurationSeconds - 12) < .001 &&
              fitted->startSeconds == imported.startSeconds && m_controller.tempo() == 150,
              "context-menu Auto Warp fits the clicked clip to the project tempo without moving it");
        const auto* other = m_controller.audioClip(track.id, second.id);
        check(other && other->warp == sibling.warp && other->durationSeconds == sibling.durationSeconds &&
              !m_sampleEditors.contains(trackId + '/' + importedId),
              "Auto Warp leaves other selected clips and the closed Warp Editor untouched");
        check(m_controller.undoDepth() == beforeAutoWarp + 1, "context-menu Auto Warp creates one Undo entry");
        m_controller.undo();
        const auto* undone = m_controller.audioClip(track.id, imported.id);
        check(undone && undone->warp.empty() && undone->durationSeconds == imported.durationSeconds,
              "Undo reverses Auto Warp while retaining the imported clip");
        m_controller.redo();
        openWarpEditor(trackId, importedId);
        auto* enabled = m_warpEditor->findChild<QToolButton*>("WarpEnabled");
        if (enabled) enabled->click();
        const auto* disabled = m_controller.audioClip(track.id, imported.id);
        check(disabled && !disabled->warp.enabled && std::abs(disabled->durationSeconds - 12) < .001 &&
              disabled->startSeconds == imported.startSeconds,
              "switching Warp off in its editor restores original duration at the same position");
        openWarpEditor(trackId, importedId);
        check(enabled && !enabled->isChecked(), "reopening Warp Editor respects disabled automatic fitting");

        int samplerRequests = 0, warpRequests = 0;
        const auto samplerConnection = connect(m_timeline, &TimelineWidget::openSampleEditorRequested, this,
            [&](const QString&, const QString& id) { if (id == importedId) ++samplerRequests; });
        const auto warpConnection = connect(m_timeline, &TimelineWidget::openWarpEditorRequested, this,
            [&](const QString&, const QString&) { ++warpRequests; });
        m_sampleEditors.value(trackId + '/' + importedId)->showSampler();
        const QPoint sampleAt(int((imported.startSeconds + .4 - m_timeline->horizontalScrollForTest()) *
                                   m_timeline->pixelsPerSecondForTest()), m_timeline->laneCentreForTest(0));
        QMouseEvent twice(QEvent::MouseButtonDblClick, QPointF(sampleAt), QPointF(m_timeline->mapToGlobal(sampleAt)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(m_timeline, &twice);
        check(samplerRequests == 1 && warpRequests == 0, "double-click still opens the sampler and does not request Warp");
        disconnect(samplerConnection); disconnect(warpConnection);
    }
    const auto windows = m_sampleEditors.values();
    for (auto* window : windows) window->close();
    m_controller.restoreProject(original, "Restore after Warp test"); syncViews(); setMixerVisible(true);
    return ok;
}
