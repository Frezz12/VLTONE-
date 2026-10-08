#include "WarpEditorWidget.hpp"
#include "EngineController.hpp"
#include "Theme.hpp"
#include "Typography.hpp"
#include "WaveformPaint.hpp"
#include "Recording/RecordingEngine.hpp"
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QElapsedTimer>
#include <QTranslator>
#include <QSettings>
#include <QInputDialog>
#include <QTemporaryDir>
#include <QtTest/QTest>
#include <cstdio>

class WarpPaintCounter : public QObject {
public:
    int paints = 0;
    bool eventFilter(QObject*, QEvent* event) override { if (event->type() == QEvent::Paint) ++paints; return false; }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("VLTONE-Warp-Test"); app.setApplicationName("Warp-Test");
    ui::initializeApplicationFonts();
    ThemeManager::instance().apply();
    int failures = 0;
    const auto check = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); failures += !ok; };
    QTemporaryDir directory;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    const auto file = directory.filePath("drums.wav").toStdString();
    audio::AudioBuffer audio(2, 96000);
    for (int i = 0; i < 96000; ++i) {
        const int age = i % 12000;
        const float sample = age < 3600 ? float(.7 * std::exp(-age / 800.) * std::cos(age * .024)) : 0;
        audio.getChannel(0)[i] = sample; audio.getChannel(1)[i] = sample;
    }
    audio::AudioRecorder recorder; recorder.initialize(48000, 2); recorder.writeWAVFile(file, audio, 48000);
    daw::EngineController controller{daw::EngineController::TestRuntime{}}; if (!controller.initialize(48000, 256, false)) return 1;
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
    auto* strength = editor.findChild<QSpinBox*>("WarpStrength"); strength->setValue(50);
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
    auto assistedBase = initial;
    assistedBase.markers[1].targetBeats = 1.13;
    controller.setClipWarp(track.id, clip.id, assistedBase); editor.refresh();
    editor.findChild<QSpinBox*>("WarpTolerance")->setValue(0);
    strength->setValue(50);
    const auto previewDepth = controller.undoDepth(); editor.previewAlignment();
    check(controller.warpPreviewActive() && state() == assistedBase && controller.undoDepth() == previewDepth,
          "assistant previews without changing the model or history");
    const auto halfPreview = *controller.warpPreviewMap();
    strength->setValue(100); strength->setValue(50);
    check(*controller.warpPreviewMap() == halfPreview, "GUI strength changes always start from confirmed timing");
    auto* beforeButton = editor.findChild<QToolButton*>("WarpBefore"); auto* afterButton = editor.findChild<QToolButton*>("WarpAfter");
    QTest::mouseClick(beforeButton, Qt::LeftButton); check(beforeButton->isChecked() && !afterButton->isChecked(), "Before comparison is exclusive");
    QTest::mouseClick(afterButton, Qt::LeftButton);
    QTest::keyClick(canvas, Qt::Key_Escape);
    check(!controller.warpPreviewActive() && state() == assistedBase && controller.undoDepth() == previewDepth, "Escape cancels assistant audition exactly");
    editor.previewAlignment(); editor.applyPreview();
    check(state() == halfPreview && controller.undoDepth() == previewDepth + 1, "assistant Apply adds one history item");
    controller.undo(); editor.refresh(); check(state() == assistedBase, "global Undo restores assistant timing");
    editor.previewAlignment(); controller.setTempo(125); editor.refresh();
    check(!controller.warpPreviewActive() && editor.findChild<QWidget*>("WarpPreviewBar")->isHidden(), "tempo change dismisses stale GUI proposals");
    controller.undo(); editor.refresh();
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, point(1.13));
    auto* target = editor.findChild<QDoubleSpinBox*>("WarpMarkerTarget");
    target->setValue(2.2); QMetaObject::invokeMethod(target, "editingFinished");
    check(std::abs(state().markers[1].targetBeats - 1.2) < 1e-5, "inspector edits a marker numerically");
    auto rangeStart = point(.75); rangeStart.setY(10); auto rangeEnd = point(2.75); rangeEnd.setY(10);
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, rangeStart); QTest::mouseMove(canvas, rangeEnd); QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, rangeEnd);
    check(std::abs(editor.findChild<QDoubleSpinBox*>("WarpRangeStart")->value() - 1.75) < .02 &&
          std::abs(editor.findChild<QDoubleSpinBox*>("WarpRangeEnd")->value() - 3.75) < .02, "ruler gesture selects an independent processing range");
    editor.resize(720, 360); QTest::qWait(40);
    check(editor.width() == 720 && editor.findChild<QPushButton*>("WarpFit")->mapTo(&editor, QPoint()).x() + editor.findChild<QPushButton*>("WarpFit")->width() <= editor.width(), "compact toolbar fits without horizontal overflow");
    auto* inspector = editor.findChild<QScrollArea*>("WarpInspector"); auto* settings = editor.findChild<QToolButton*>("WarpInspectorButton");
    QTest::mouseClick(settings, Qt::LeftButton); check(inspector->isHidden(), "compact inspector closes immediately");
    QTest::mouseClick(settings, Qt::LeftButton); check(inspector->isVisible(), "compact inspector opens as an overlay");
    editor.previewAlignment(); editor.hide();
    check(!controller.warpPreviewActive() && state().markers.size() == assistedBase.markers.size(), "hiding the panel cancels uncommitted audition");
    editor.resize(1100, 440); editor.show();
    QTest::qWait(400);
    WarpPaintCounter paintCounter; canvas->installEventFilter(&paintCounter);
    QTest::qWait(100); paintCounter.paints = 0;
    const auto waveformBefore = ui::waveformPaintStatsForTest();
    for (int i = 0; i < 12; ++i) { controller.seekSeconds(.1 + i * .1); QTest::qWait(20); }
    const auto waveformAfter = ui::waveformPaintStatsForTest();
    check(paintCounter.paints > 0 && waveformBefore.tileBuilds == waveformAfter.tileBuilds && waveformBefore.tileHits == waveformAfter.tileHits,
          "playhead frames reuse the static waveform instead of repainting it");
    QTest::qWait(40); paintCounter.paints = 0; QTest::qWait(120);
    check(paintCounter.paints == 0, "stationary editor does not repaint on a polling timer");
    editor.hide(); paintCounter.paints = 0; controller.seekSeconds(0); QTest::qWait(80);
    check(paintCounter.paints == 0, "hidden editor has no playhead paints"); editor.show();
    canvas->removeEventFilter(&paintCounter);
    if (argc > 1) {
        editor.findChild<QSpinBox*>("WarpSwing")->setValue(64); editor.previewAlignment(); QTest::qWait(100);
        editor.grab().save(QString::fromLocal8Bit(argv[1])); editor.cancelPreview();
    }
    controller.removeClip(track.id, clip.id); editor.refresh();
    check(editor.findChild<QWidget*>("WarpCanvas") && !editor.findChildren<QCheckBox*>().front()->isEnabled(), "deleted clip shows inactive editor");
    editor.clearClip();
    auto extractionProject = project;
    daw::TrackModel midiTrack; midiTrack.id = daw::newUuid(); midiTrack.kind = daw::TrackKind::Midi; midiTrack.name = "Groove reference";
    daw::ClipModel midiClip; midiClip.id = daw::newUuid(); midiClip.kind = daw::ClipKind::Midi; midiClip.name = "Human MIDI"; midiClip.durationSeconds = 2;
    for (double beat : {.02, .28, .51, .79, 1.02, 1.29, 1.51, 1.78}) { daw::NoteModel note; note.id = daw::newUuid(); note.startBeats = beat; note.lengthBeats = .1; midiClip.notes.push_back(note); }
    midiTrack.clips.push_back(midiClip); extractionProject.tracks.push_back(midiTrack); controller.restoreProject(extractionProject, "Groove test");
    editor.setClip(QString::fromStdString(track.id), QString::fromStdString(clip.id));
    editor.findChild<QComboBox*>("WarpGrooveSource")->setCurrentIndex(1);
    QTimer::singleShot(0, [] { if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) { dialog->setTextValue("Human test groove"); dialog->accept(); } });
    editor.findChild<QPushButton*>("WarpSaveGroove")->click();
    check(editor.findChild<QComboBox*>("WarpGroove")->currentText() == "Human test groove", "MIDI groove extraction selects the saved user preset");
    {
        WarpEditorWidget reopened(&controller);
        check(reopened.findChild<QComboBox*>("WarpGroove")->findText("Human test groove") >= 0, "user groove survives reopening the editor");
    }
    const auto neutralGroove = controller.audioClip(track.id, clip.id)->warp;
    editor.previewAlignment(); editor.cancelPreview();
    check(controller.audioClip(track.id, clip.id)->warp == neutralGroove, "extracted groove audition stays reversible");
    controller.newProject(false); editor.refresh(); check(editor.clipId().isEmpty(), "new project clears the pinned Warp clip");
    editor.clearClip();
    if (argc > 2) {
        QTranslator russian;
        check(russian.load(QString::fromLocal8Bit(argv[2])), "Russian translation bundle loads");
        app.installTranslator(&russian);
        controller.restoreProject(project, "Visual fixture");
        WarpEditorWidget localized(&controller); localized.resize(1100, 440); localized.show(); localized.setClip(QString::fromStdString(track.id), QString::fromStdString(clip.id));
        QTest::qWait(500); localized.previewAlignment(); QTest::qWait(100);
        const auto output = QString::fromLocal8Bit(argv[1]);
        localized.grab().save(output + ".ru.png");
        localized.resize(720, 400); QTest::qWait(100);
        check(localized.width() == 720 && localized.findChild<QPushButton*>("WarpSuggest")->mapTo(&localized, QPoint()).x() + localized.findChild<QPushButton*>("WarpSuggest")->width() <= localized.width(), "Russian compact controls fit the panel");
        localized.grab().save(output + ".ru-compact.png");
        ThemeManager::instance().setThemeId("light", false); QTest::qWait(80);
        localized.grab().save(output + ".ru-light.png");
        localized.cancelPreview(); localized.clearClip(); app.removeTranslator(&russian);
    }
    return failures ? 1 : 0;
}
