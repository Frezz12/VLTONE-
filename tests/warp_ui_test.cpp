#include "WarpEditorWidget.hpp"
#include "EngineController.hpp"
#include "Theme.hpp"
#include "Recording/RecordingEngine.hpp"
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QtTest/QTest>
#include <cstdio>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("VLTONE-Warp-Test"); app.setApplicationName("Warp-Test");
    ThemeManager::instance().apply();
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; };
    QTemporaryDir directory;
    const auto file = directory.filePath("drums.wav").toStdString();
    audio::AudioBuffer audio(2, 96000);
    for (int i = 0; i < 96000; ++i) {
        const int age = i % 12000;
        const float sample = age < 3600 ? float(.7 * std::exp(-age / 800.) * std::cos(age * .024)) : 0;
        audio.getChannel(0)[i] = sample; audio.getChannel(1)[i] = sample;
    }
    audio::AudioRecorder recorder; recorder.initialize(48000, 2); recorder.writeWAVFile(file, audio, 48000);
    daw::EngineController controller; if (!controller.initialize(48000, 256, false)) return 1;
    daw::ProjectModel project; project.tempo = 120;
    daw::TrackModel track; track.id = daw::newUuid(); track.kind = daw::TrackKind::Audio; track.name = "Drums";
    daw::ClipModel clip; clip.id = daw::newUuid(); clip.filePath = file; clip.kind = daw::ClipKind::Audio;
    clip.name = "Acoustic loop"; clip.durationSeconds = 2; clip.channels = 2;
    track.clips.push_back(clip); project.tracks.push_back(track); controller.restoreProject(project, "Test");
    WarpEditorWidget editor(&controller); editor.resize(1000, 410); editor.show();
    check(editor.setClip(QString::fromStdString(track.id), QString::fromStdString(clip.id)), "Warp opens selected clip");
    QTest::qWait(180);
    auto* canvas = editor.findChild<QWidget*>("WarpCanvas");
    const auto point = [&](double beat) { return QPoint(int(12 + beat / 4 * (canvas->width() - 24)), 92); };
    const auto state = [&] { return controller.audioClip(track.id, clip.id)->warp; };
    QTest::mouseDClick(canvas, Qt::LeftButton, Qt::NoModifier, point(1));
    check(state().markers.size() == 3, "double-click adds a real marker");
    if (state().markers.size() != 3) return 1;
    const auto initial = state(); const auto depth = controller.undoDepth();
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, point(1));
    QTest::mouseMove(canvas, point(1.5)); QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, point(1.5));
    check(std::abs(state().markers[1].targetBeats - 1.5) < .001, "mouse drag snaps marker to grid");
    check(controller.undoDepth() == depth + 1, "GUI drag commits one history item");
    controller.undo(); editor.refresh(); check(state() == initial, "global Undo restores GUI drag");
    controller.redo(); editor.refresh(); const auto moved = state();
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, point(1.5)); QTest::mouseMove(canvas, point(2));
    QTest::keyClick(canvas, Qt::Key_Escape); QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, point(2));
    check(state() == moved, "Escape cancels a live GUI drag");
    QTest::keyClick(canvas, Qt::Key_Right);
    check(std::abs(state().markers[1].targetBeats - 1.75) < .001, "keyboard moves selected marker precisely");
    QTest::keyClick(canvas, Qt::Key_Delete); check(state().markers.size() == 2, "Delete removes marker, keeps the clip");
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, point(0)); QTest::keyClick(canvas, Qt::Key_Delete);
    check(state().markers.size() == 2, "locked clip boundary cannot be deleted");
    auto off = state(); off.enabled = false; controller.setClipWarp(track.id, clip.id, off); editor.refresh();
    editor.setClip(QString::fromStdString(track.id), QString::fromStdString(clip.id));
    check(!state().enabled, "reopening the same clip respects Warp Off");
    off.enabled = true; controller.setClipWarp(track.id, clip.id, off); editor.refresh();
    QTest::mouseDClick(canvas, Qt::LeftButton, Qt::NoModifier, point(1.1));
    auto* strength = editor.findChildren<QSpinBox*>().front(); strength->setValue(50);
    const auto beforeQuantize = state(); editor.quantize();
    const double original = beforeQuantize.markers[1].targetBeats;
    check(std::abs(state().markers[1].targetBeats - (original + std::round(original / .25) * .25) * .5) < 1e-6,
          "Quantize Strength 50 percent halves the timing error");
    QTest::mouseDClick(canvas, Qt::LeftButton, Qt::NoModifier, point(2.1));
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::ControlModifier, point(state().markers[1].targetBeats));
    const auto group = state(); QTest::keyClick(canvas, Qt::Key_Right);
    check(state().markers.size() == 4 && std::abs(state().markers[1].targetBeats - group.markers[1].targetBeats - .25) < 1e-6 &&
          std::abs(state().markers[2].targetBeats - group.markers[2].targetBeats - .25) < 1e-6,
          "multiple selected markers move together without changing source anchors");
    auto contextAction = [&](const QString& label) {
        bool chosen = false;
        QTimer::singleShot(0, [&] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (auto* action : menu->actions()) if (action->text() == label) {
                    chosen = true; menu->setActiveAction(action); QTest::keyClick(menu, Qt::Key_Return); return;
                }
                menu->close();
            }
        });
        const auto at = point(state().markers[1].targetBeats);
        QContextMenuEvent event(QContextMenuEvent::Mouse, at, canvas->mapToGlobal(at));
        QApplication::sendEvent(canvas, &event); return chosen;
    };
    check(contextAction("Lock Position") && state().markers[1].locked && state().markers[2].locked,
          "context command locks all selected interior markers");
    QTest::keyClick(canvas, Qt::Key_Delete);
    check(state().markers.size() == 4, "locked markers survive Delete");
    check(contextAction("Unlock") && !state().markers[1].locked && !state().markers[2].locked, "context command unlocks the selection");
    strength->setValue(100); editor.quantize();
    check(std::abs(state().markers[1].targetBeats / .25 - std::round(state().markers[1].targetBeats / .25)) < 1e-6 &&
          std::abs(state().markers[2].targetBeats / .25 - std::round(state().markers[2].targetBeats / .25)) < 1e-6,
          "Quantize Strength 100 percent puts selected markers on the grid");
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, point(state().markers[1].targetBeats));
    QTest::mouseMove(canvas, point(4)); QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, point(4));
    check(daw::validWarp(state()) && state().markers[2].targetBeats < state().markers.back().targetBeats,
          "drag cannot cross another marker or the fixed endpoint");
    QTest::qWait(100);
    if (argc > 1) editor.grab().save(QString::fromLocal8Bit(argv[1]));
    controller.removeClip(track.id, clip.id); editor.refresh();
    check(editor.findChild<QWidget*>("WarpCanvas") && !editor.findChildren<QCheckBox*>().front()->isEnabled(), "deleted clip shows inactive editor");
    editor.clearClip();
    return failures ? 1 : 0;
}
