#include "VocalAlignmentWindow.hpp"
#include "VocalAlignmentPreview.hpp"
#include "ClipLibraryDrag.hpp"
#include "Recording/RecordingEngine.hpp"
#include "Theme.hpp"
#include "Typography.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTranslator>
#include <QDir>
#include <QFileInfo>
#include <QTest>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* label) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", label); failures += !ok; }
bool drop(QWidget* target, const QVector<ui::ClipSel>& clips, daw::EngineController& controller, QPoint point = {10,10}) {
    QMimeData mime; ui::cliplibrary::addTimelineGroup(&mime, clips, &controller, controller.projectGeneration());
    QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(target, &enter);
    QDropEvent event(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(target, &event); return event.isAccepted();
}
bool wavesReady(VocalAlignmentPreview* preview) {
    QElapsedTimer timer; timer.start();
    while (!preview->waveformsReady() && timer.elapsed() < 5000) QTest::qWait(25);
    return preview->waveformsReady();
}
void capture(QWidget& window, const QString& suffix = {}) {
    const auto path = qEnvironmentVariable("ALIGNMENT_SCREENSHOT"); if (path.isEmpty()) return;
    const QFileInfo file(path); QDir().mkpath(file.absolutePath());
    window.grab().save(file.absolutePath() + "/" + file.completeBaseName() + suffix + ".png");
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); QApplication::setStyle("Fusion");
    app.setOrganizationName("VLTONE-Alignment-Test"); app.setApplicationName("Alignment-Test");
    ui::initializeApplicationFonts();
    ThemeManager::instance().setThemeId(qEnvironmentVariable("ALIGNMENT_THEME", "dark"), false);
    ThemeManager::instance().apply();
    QTranslator translator;
    if (qEnvironmentVariable("ALIGNMENT_LANGUAGE") == "ru" && translator.load(QCoreApplication::applicationDirPath() + "/../app/vlt_ru.qm")) app.installTranslator(&translator);
    daw::EngineController controller; if (!controller.initialize(48000,256,false)) return 1;
    QTemporaryDir dir; const auto file = dir.filePath("voice.wav").toStdString();
    audio::AudioBuffer audio(2,96000);
    for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 96000; ++i) {
        const double time = i / 48000., phase = std::fmod(time, .31), syllable = std::floor(time / .31);
        const double envelope = phase < .05 || phase > .19 + .012 * syllable ? 0 : std::min(1., (phase - .05) * 200) * std::exp(-(phase - .05) * (8 + syllable));
        audio.getChannel(ch)[i] = float(.65 * envelope * (std::sin(time * (160 + syllable * 13) * 6.2831853) + .25 * std::sin(time * 2440)));
    }
    audio::AudioRecorder recorder; recorder.initialize(48000,2); recorder.writeWAVFile(file,audio,48000);
    const auto track = controller.addTrack(daw::TrackKind::Audio,"Vocals");
    const auto ref = [&](double start) { return ui::ClipSel{QString::fromStdString(track), QString::fromStdString(controller.importAudio(file,track,start))}; };
    const auto a = ref(2), b = ref(2.1), c = ref(2.2), d = ref(2.3);
    ui::SelectionModel selection;
    selection.setClips({a}); selection.setClips({b,a}); selection.setClips({c,a,b}); selection.setClips({b,a,c});
    check(selection.clips().front() == b && selection.clipsInSelectionOrder() == QVector<ui::ClipSel>{a,b,c}, "primary selection does not change chronological guide");
    VocalAlignmentWindow window(&controller,&selection); window.setAttribute(Qt::WA_DeleteOnClose,false); window.show(); QApplication::processEvents();
    check(!window.isModal() && window.guide() == a && window.targets() == QVector<ui::ClipSel>{b,c}, "nonmodal window takes the first selected clip as guide");
    auto* preview = window.findChild<VocalAlignmentPreview*>();
    check(preview && wavesReady(preview), "guide and selected target have waveforms before running alignment");
    bool accessibleIcons = true;
    for (auto* button : window.findChildren<QToolButton*>()) accessibleIcons &= !button->icon().isNull() && !button->accessibleName().isEmpty() && !button->toolTip().isEmpty() && button->focusPolicy() == Qt::StrongFocus;
    check(accessibleIcons, "icon actions have tooltips, accessible names and keyboard focus");
    capture(window, "-before");
    window.findChild<QToolButton*>("AlignmentAdvanced")->setChecked(true); QApplication::processEvents();
    check(window.findChild<QPushButton*>("AlignmentRun")->visibleRegion().boundingRect().height() > 15, "run button remains visible with Advanced expanded");
    window.findChild<QToolButton*>("AlignmentAdvanced")->setChecked(false);
    selection.setClips({d});
    check(window.guide() == a && window.targets().size() == 2, "later timeline selection does not rebuild the window");
    auto* guide = window.findChild<QLabel*>("AlignmentGuide");
    auto* list = window.findChild<QListWidget*>("AlignmentTargets");
    auto* add = window.findChild<QToolButton*>("AlignmentAddTargets");
    check(drop(add,{a,b,c,d},controller) && window.targets().size() == 3, "group drop adds targets without duplicates or self-alignment");
    check(drop(guide,{b},controller) && window.guide() == b && !window.targets().contains(b), "guide drop replaces reference and removes self-target");
    check(drop(list->viewport(),{a},controller,list->visualItemRect(list->item(0)).center()) && window.targets().front() == a, "dropping one clip onto a row replaces that target");
    check(!drop(list->viewport(),{a,c},controller,list->visualItemRect(list->item(0)).center()), "group replacement of a single row is rejected");
    check(controller.audioClip(track,a.clipId.toStdString())->startSeconds == 2 && controller.audioClip(track,d.clipId.toStdString())->startSeconds == 2.3, "reference drops do not move source clips");
    list->setCurrentRow(0); window.findChild<QToolButton*>("AlignmentPromote")->click();
    check(window.guide() == a && window.targets().contains(b), "set-as-guide swaps roles without losing the former guide");
    list->setCurrentRow(0); QSignalSpy editor(&window,&VocalAlignmentWindow::warpEditorRequested); window.findChild<QToolButton*>("AlignmentWarp")->click();
    check(editor.count() == 1, "manual Warp editor action identifies the selected clip");
    window.findChild<QToolButton*>("AlignmentTakeSelection")->click();
    check(window.guide() == d && window.targets().isEmpty(), "one selected clip fills only the guide slot");
    selection.clear(); window.takeSelection(); check(window.guide().clipId.isEmpty() && window.targets().isEmpty(), "empty selection opens empty slots");
    capture(window, "-empty");
    selection.setClips({a,b,c},{a,b,c}); window.takeSelection();
    window.findChild<QComboBox*>("AlignmentMethod")->setCurrentIndex(2);
    QSignalSpy applied(&window,&VocalAlignmentWindow::alignmentApplied);
    const auto depth = controller.undoDepth(); window.findChild<QPushButton*>("AlignmentRun")->click();
    check(applied.wait(15000) && controller.undoDepth() == depth+1 && window.isVisible(), "background run applies one Undo and leaves window open");
    check(wavesReady(preview) && preview->hasResult() && std::abs(preview->projectTimeForSource(1,.1) - 2.2) < 1e-6 &&
        std::abs(preview->projectTimeForSource(2,.1) - 2.1) < .001, "comparison preserves before timing and displays applied after timing");
    const double fitSpan = preview->visibleDuration();
    window.findChild<QToolButton*>("AlignmentZoomIn")->click();
    check(preview->visibleDuration() < fitSpan, "zoom controls magnify all three waveforms together");
    preview->setFocus(); QTest::keyClick(preview, Qt::Key_Home);
    check(std::abs(preview->visibleDuration() - fitSpan) < 1e-9, "keyboard Home restores the shared full range");
    capture(window);
    list->setCurrentRow(1);
    check(std::abs(preview->projectTimeForSource(1,.1) - 2.3) < 1e-6, "selecting another target switches its original waveform");
    list->setCurrentRow(0);
    window.resize(800,580); window.findChild<QToolButton*>("AlignmentAdvanced")->setChecked(true); QApplication::processEvents();
    const auto* detail = window.findChild<QLabel*>("AlignmentResultDetail");
    const int waveBottom = preview->mapTo(&window,QPoint(0,preview->height())).y();
    const int detailTop = detail->mapTo(&window,QPoint()).y();
    check(preview->height() >= 220 && waveBottom <= detailTop && window.findChild<QPushButton*>("AlignmentRun")->visibleRegion().boundingRect().height() > 15,
        "small window keeps waveform, details and controls separate with Advanced expanded");
    check(window.findChild<QWidget*>("AlignmentTolerance")->visibleRegion().boundingRect().height() >= 20, "expanding Advanced reveals its first control without manual scrolling");
    capture(window, "-compact"); const int expandedWaveHeight = preview->height();
    window.findChild<QToolButton*>("AlignmentAdvanced")->setChecked(false); QApplication::processEvents();
    check(preview->height() > expandedWaveHeight, "collapsing Advanced immediately returns space to the waveforms");
    window.resize(1120,720);
    window.findChild<QSlider*>("AlignmentDensity")->setValue(0); window.findChild<QPushButton*>("AlignmentRun")->click();
    check(applied.wait(15000) && controller.audioClip(track,b.clipId.toStdString())->startSeconds == 2.1, "zero-density rerun restores original placement");
    check(preview->hasResult() && std::abs(preview->projectTimeForSource(1,.1) - preview->projectTimeForSource(2,.1)) < 1e-9, "zero density displays an unchanged before/after comparison");
    controller.undo(); list->setCurrentRow(1); QTest::qWait(150);
    check(!preview->hasResult() && !window.findChild<QPushButton*>("AlignmentRun")->isEnabled(), "external undo clears stale result and asks for new selection");
    window.takeSelection();
    const auto beforeCancel = controller.undoDepth(); window.findChild<QSlider*>("AlignmentDensity")->setValue(100); window.findChild<QPushButton*>("AlignmentRun")->click();
    window.close(); QTest::qWait(250);
    check(controller.undoDepth() == beforeCancel, "closing during analysis prevents application");
    std::vector<daw::EngineController::VocalAlignmentClip> sources;
    controller.prepareVocalAlignment({{track,a.clipId.toStdString()},{track,b.clipId.toStdString()}}, sources);
    if (sources.size() == 2) {
        VocalAlignmentPreview mapped; auto target = sources[1].source;
        target.startSeconds = 5; target.tempo = 120; target.durationSeconds = 2;
        target.baseline.markers = {{"first",.1,0,true},{"middle",.9,2,false},{"last",1.8,4,true}};
        daw::alignment::AlignmentProposal result; result.map = target.baseline; result.map.markers[1].targetBeats = 1.8;
        result.startSeconds = 4.9; result.durationSeconds = 2;
        mapped.setSnapshot(sources[0].source,target,result);
        check(std::abs(mapped.projectTimeForSource(1,.9) - 6) < 1e-9 && std::abs(mapped.projectTimeForSource(2,.9) - 5.8) < 1e-9,
            "comparison composes trim, clip position and nonuniform Warp coordinates");
    } else check(false, "prepare source for Warp comparison");
    const auto otherTrack = controller.addTrack(daw::TrackKind::Audio, "Double");
    const ui::ClipSel other{QString::fromStdString(otherTrack),
        QString::fromStdString(controller.importAudio(file, otherTrack, 2.15))};
    controller.setClipStartSeconds(track,b.clipId.toStdString(),2.1);
    controller.setClipStartSeconds(track,c.clipId.toStdString(),2.2);
    selection.setClips({a,b},{a,b});
    VocalAlignmentWindow batch(&controller,&selection); batch.setAttribute(Qt::WA_DeleteOnClose,false);
    auto* batchList = batch.findChild<QListWidget*>("AlignmentTargets");
    auto* batchRun = batch.findChild<QPushButton*>("AlignmentRun");
    batch.findChild<QComboBox*>("AlignmentMethod")->setCurrentIndex(2);
    QSignalSpy batchApplied(&batch,&VocalAlignmentWindow::alignmentApplied);
    batchRun->click();
    check(batchApplied.wait(15000), "initial single-target run completes");
    selection.setClips({c,other},{c,other});
    batch.findChild<QToolButton*>("AlignmentAddTargets")->click();
    selection.setClips({d});
    batchList->setCurrentRow(1);
    const auto batchDepth = controller.undoDepth();
    batchRun->click();
    check(batchApplied.wait(15000) && controller.undoDepth() == batchDepth + 1,
        "adding targets after a run applies the entire batch in one Undo");
    check(std::abs(controller.audioClip(track,b.clipId.toStdString())->startSeconds - 2) < .001 &&
        std::abs(controller.audioClip(track,c.clipId.toStdString())->startSeconds - 2) < .001 &&
        std::abs(controller.audioClip(otherTrack,other.clipId.toStdString())->startSeconds - 2) < .001,
        "Align changes every added target across tracks regardless of the selected row");
    bool allReported = batchList->count() == 3;
    for (int row = 0; row < batchList->count(); ++row)
        allReported &= !batchList->item(row)->text().endsWith("\n" + QCoreApplication::translate("VocalAlignmentWindow", "Ready"));
    check(allReported, "every added target receives a result instead of remaining Ready");
    check(controller.audioClip(track,a.clipId.toStdString())->startSeconds == 2 &&
        controller.audioClip(track,d.clipId.toStdString())->startSeconds == 2.3,
        "batch leaves the guide and clips outside the target list unchanged");
    controller.undo();
    check(controller.audioClip(track,c.clipId.toStdString())->startSeconds == 2.2 &&
        controller.audioClip(otherTrack,other.clipId.toStdString())->startSeconds == 2.15,
        "Undo restores all newly added targets across tracks");
    batch.close();
    controller.shutdown(); std::printf("%d failures\n", failures); return failures ? 1 : 0;
}
