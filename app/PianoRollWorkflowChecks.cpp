#include "PianoRollWindow.hpp"
#include "PianoRollTools.hpp"
#include "TimelineWidget.hpp"
#include "InternalEditorFrame.hpp"
#include "EngineController.hpp"
#include "Internal/SamplerInstance.hpp"
#include "Internal/SamplerParams.hpp"
#include "UiConstants.hpp"
#include "graphics/WorkspaceSurface.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QQuickWindow>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <cstdio>
#include <algorithm>

namespace {
bool writeWave(const QString& path, qint16 amplitude) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4); out << quint32(36 + 2048);
    out.writeRawData("WAVEfmt ", 8); out << quint32(16) << quint16(1) << quint16(1)
        << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
    out.writeRawData("data", 4); out << quint32(2048);
    for (int i = 0; i < 1024; ++i) out << qint16(i % 64 < 32 ? amplitude : -amplitude);
    return out.status() == QDataStream::Ok;
}
}

bool PianoRollWindow::checkWorkflowsForTest(const QString& images) {
    daw::EngineController controller{daw::EngineController::TestRuntime{}};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Sampler phrase");
    const auto clipId = controller.addMidiClip(track, 0, 16);
    QWidget workspace;
    workspace.resize(1200, 740);
    InternalEditorFrame pianoFrame("checks/pianoWorkflows/piano", &workspace);
    PianoRollWindow window(&controller, &pianoFrame);
    window.resize(1080, 570);
    window.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    pianoFrame.setContent(&window);
    QPointer<InternalEditorFrame> chordFrame;
    connect(&window, &PianoRollWindow::internalWindowRequested, &workspace,
            [&](QWidget* content, const QString&) {
        if (!chordFrame) {
            QSettings().remove("checks/pianoWorkflows/chord");
            chordFrame = new InternalEditorFrame("checks/pianoWorkflows/chord", &workspace);
            chordFrame->setContent(content);
        }
        chordFrame->present();
    });
    workspace.show();
    pianoFrame.present();
    QApplication::processEvents();
    bool ok = PianoRollView::checkVelocityRampForTest(images);
    const auto check = [&](bool pass, const char* label) {
        std::printf("%s piano workflow: %s\n", pass ? "PASS" : "FAIL", label);
        ok &= pass;
    };
    const auto notes = [&] { return daw::midiNotes(*window.m_view->clip()); };
    window.m_chordAction->trigger();
    QApplication::processEvents();
    auto* dialog = window.m_chordDialog;
    if (!dialog || !chordFrame) return false;
    auto* source = dialog->findChild<QComboBox*>("ChordSource");
    auto* type = dialog->findChild<QComboBox*>("ChordType");
    auto* root = dialog->findChild<QComboBox*>("ChordRoot");
    auto* inversion = dialog->findChild<QSpinBox*>("ChordInversion");
    auto* start = dialog->findChild<QDoubleSpinBox*>("ChordStart");
    auto* length = dialog->findChild<QDoubleSpinBox*>("ChordLength");
    auto* velocity = dialog->findChild<QSpinBox*>("ChordVelocity");
    auto* bass = dialog->findChild<QCheckBox*>("ChordBass");
    auto* buttons = dialog->findChild<QDialogButtonBox*>();
    if (!source || !type || !root || !inversion || !start || !length ||
        !velocity || !bass || !buttons) return false;
    check(source->currentIndex() == 1 && window.m_view->m_preview && window.m_view->m_preview->size() == 3 && notes().empty(),
          "empty clip immediately previews a new chord without changing the document");
    root->setCurrentIndex(62);
    type->setCurrentIndex(type->findData(int(daw::miditools::ChordParams::Type::Minor7)));
    inversion->setValue(1);
    start->setValue(1.25); length->setValue(2.5); velocity->setValue(87);
    bass->setChecked(true);
    const auto depth = controller.undoDepth();
    buttons->button(QDialogButtonBox::Apply)->click();
    const auto generated = notes();
    check(generated.size() == 5 && generated[0].pitch == 50 && generated[1].pitch == 74 &&
          generated[2].pitch == 65 && generated[0].startBeats == 1.25 &&
          generated[0].lengthBeats == 2.5 && generated[0].velocity == 87 &&
          controller.undoDepth() == depth + 1,
          "Apply flushes the latest pitch, inversion, bass, time, length and velocity in one undo");
    controller.undo(); check(notes().empty(), "chord undo restores an empty clip");
    controller.redo(); check(notes() == generated, "chord redo preserves note identities");
    check(chordFrame->width() < 580 && chordFrame->height() < 420,
          "hosted chord editor opens compactly");
    const auto* shortcut = window.findChild<QToolButton*>("PianoRollBuildChordsButton");
    const auto* scope = window.findChild<QComboBox*>("PianoRollSlideScope");
    check(shortcut && shortcut->parentWidget()->objectName() == "PianoRollFooter" &&
          scope && scope->width() < 140 && window.m_clipSelector->width() <= 280,
          "chord shortcut lives in footer and selectors stay compact");
    QStyleOptionComboBox comboStyle;
    comboStyle.initFrom(window.m_clipSelector);
    const auto textArea = window.m_clipSelector->style()->subControlRect(
        QStyle::CC_ComboBox, &comboStyle, QStyle::SC_ComboBoxEditField, window.m_clipSelector);
    check(textArea.width() >= window.m_clipSelector->fontMetrics().horizontalAdvance(
              window.m_clipSelector->currentText()), "compact clip selector fits its complete caption");
    if (!images.isEmpty()) {
        QDir().mkpath(images);
        QApplication::processEvents();
        window.grab().save(QDir(images).filePath("piano.png"));
        chordFrame->grab().save(QDir(images).filePath("chord-new.png"));
    }
    dialog->close(); chordFrame->hide();
    controller.undo();
    daw::NoteModel base;
    base.id = daw::newUuid(); base.pitch = 60; base.startBeats = 0; base.lengthBeats = 2;
    daw::NoteModel untouched = base;
    untouched.id = daw::newUuid(); untouched.pitch = 55; untouched.startBeats = 4;
    controller.setClipNotes(track, clipId, {base, untouched}, "Fixture");
    window.m_view->m_selected = {QString::fromStdString(base.id)};
    window.m_chordAction->trigger();
    check(source->currentIndex() == 0 && !start->isVisible(),
          "existing selected notes open the harmonization controls");
    type->setCurrentIndex(type->findData(int(daw::miditools::ChordParams::Type::Major)));
    inversion->setValue(0); bass->setChecked(false);
    window.flushToolPreview(dialog);
    if (!images.isEmpty()) {
        QApplication::processEvents();
        chordFrame->grab().save(QDir(images).filePath("chord-existing.png"));
    }
    buttons->button(QDialogButtonBox::Apply)->click();
    const auto harmonized = notes();
    check(harmonized.size() == 4 && std::find(harmonized.begin(), harmonized.end(), untouched) != harmonized.end(),
          "harmonizing the selection preserves other notes");
    controller.undo();
    type->setCurrentIndex(type->findData(int(daw::miditools::ChordParams::Type::Minor)));
    window.flushToolPreview(dialog);
    dialog->close(); chordFrame->hide();
    check(!window.m_view->hasPreview() && notes() == daw::miditools::Notes({base, untouched}),
          "closing discards the unapplied preview");

    window.m_view->setPixelsPerBeat(150);
    window.m_view->setScrollX(1300);
    controller.seekSeconds(0);
    controller.play();
    window.m_view->refreshPlayheadFrame();
    const double manuallyScrolled = window.m_view->scrollX();
    check(controller.isPlaying() && manuallyScrolled == 1300,
          "playback never returns the viewport to an offscreen playhead");
    window.m_view->setScrollX(2000);
    controller.seekSeconds(1);
    window.m_view->refreshPlayheadFrame();
    check(window.m_view->scrollX() == 2000, "manual navigation stays free while playing");
    controller.stop();

    std::unique_ptr<ui::graphics::WorkspaceSurface> gpu;
    if (qEnvironmentVariableIsSet("DAW_PIANO_WORKFLOW_GPU")) {
        workspace.hide();
        gpu = std::make_unique<ui::graphics::WorkspaceSurface>(&workspace);
        workspace.show();
        QElapsedTimer ready;
        ready.start();
        while (!gpu->quickWindow()->isSceneGraphInitialized() && ready.elapsed() < 3000) {
            QApplication::processEvents();
            QThread::msleep(1);
        }
        check(gpu->quickWindow()->isSceneGraphInitialized() &&
              !gpu->quickWindow()->grabWindow().isNull(), "native GPU scene is active for sample-drop routing");
    }
    QTemporaryDir samples;
    const QString first = samples.filePath("first.wav"), second = samples.filePath("second.wav");
    if (!writeWave(first, 2000) || !writeWave(second, 4000) ||
        !controller.loadInstrumentSampler(track, first.toStdString())) return false;
    namespace smp = daw::plugins::sampler;
    const auto slot = controller.project().findTrack(track)->instrument.id;
    auto* sampler = controller.samplerInstance(track, slot);
    sampler->setParameter(smp::indexOf(smp::Param::AmpAttack), 0.15);
    sampler->setParameter(smp::indexOf(smp::Param::Pan), -0.35);
    sampler->setParameter(smp::indexOf(smp::Param::LoopMode), 1);
    sampler->setParameter(smp::indexOf(smp::Param::RootNote), 57);
    sampler->setParameter(smp::indexOf(smp::SlideParam::TimeMs), 250);
    const auto parameters = [&](smp::SamplerInstance* instance) {
        std::vector<double> values;
        for (unsigned i = 0; i < smp::kParameterCount; ++i) values.push_back(instance->parameterValue(i));
        return values;
    };
    const auto beforeParameters = parameters(sampler);
    const auto beforeNotes = notes();
    auto* view = window.m_view;
    const QPoint gridPoint(int(view->keyboardWidth() + 70), int(ui::kRulerHeight + 60));
    const auto dropSample = [&](const QString& path, QPoint point) {
        QApplication::processEvents();
        QObject* receiver = gpu ? static_cast<QObject*>(gpu->quickWindow()) : view;
        if (gpu) point = view->mapTo(&workspace, point);
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(path)});
        QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(receiver, &enter);
        if (enter.isAccepted() && !images.isEmpty()) {
            if (gpu) {
                gpu->invalidate(); QApplication::processEvents();
                gpu->quickWindow()->grabWindow().save(QDir(images).filePath("drop-gpu.png"));
            } else view->grab().save(QDir(images).filePath("drop.png"));
        }
        QDropEvent drop(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(receiver, &drop);
        return enter.isAccepted() && drop.isAccepted();
    };
    const auto sampleDepth = controller.undoDepth();
    check(dropSample(second, gridPoint) && sampler == controller.samplerInstance(track, slot) &&
          sampler->samplePath() == second.toStdString() && parameters(sampler) == beforeParameters &&
          notes() == beforeNotes && controller.undoDepth() == sampleDepth + 1,
          "grid drop replaces only the sample, preserving all SMP/INS/slide parameters and notes");
    controller.undo();
    check(sampler->samplePath() == first.toStdString() && parameters(sampler) == beforeParameters,
          "sample replacement undo restores the file without changing parameters");
    controller.redo();
    check(sampler->samplePath() == second.toStdString(), "sample replacement redo restores the new file");
    check(!dropSample(first, QPoint(5, 5)) && sampler->samplePath() == second.toStdString(),
          "ruler and keyboard are not sample drop targets");
    const QString broken = samples.filePath("broken.wav");
    QFile invalid(broken); invalid.open(QIODevice::WriteOnly); invalid.write("not audio"); invalid.close();
    const auto invalidDepth = controller.undoDepth();
    check(!dropSample(broken, gridPoint) && sampler->samplePath() == second.toStdString() &&
          parameters(sampler) == beforeParameters && controller.undoDepth() == invalidDepth,
          "a corrupt sample leaves the current sound and history intact");
    const auto instrument = controller.pluginManager().find(daw::plugins::Format::Internal, "daw.sampler");
    const auto pattern = controller.addPattern("Pattern drop");
    const auto sound = controller.addPatternInstrument(pattern, *instrument);
    const auto* soundTrack = controller.project().findTrack(sound);
    const auto sourceClip = soundTrack->clips.front().id;
    const auto soundSlot = soundTrack->instrument.id;
    window.setClip(QString::fromStdString(sound), QString::fromStdString(sourceClip));
    check(dropSample(first, gridPoint) &&
          controller.samplerInstance(sound, soundSlot)->samplePath() == first.toStdString() &&
          sampler->samplePath() == second.toStdString(),
          "a pattern piano roll replaces only its open sound, leaving the previous instrument alone");
    const auto other = controller.addTrack(daw::TrackKind::Midi, "No sampler");
    const auto otherClip = controller.addMidiClip(other, 0, 8);
    window.setClip(QString::fromStdString(other), QString::fromStdString(otherClip));
    const auto rejectDepth = controller.undoDepth();
    check(!dropSample(first, gridPoint) && controller.undoDepth() == rejectDepth &&
          controller.project().findTrack(other)->instrument.id.empty(),
          "non-sampler tracks reject drops without installing an instrument");
    ok &= TimelineWidget::checkMidiClipOpeningForTest();
    return ok;
}

bool TimelineWidget::checkMidiClipOpeningForTest() {
    daw::EngineController controller{daw::EngineController::TestRuntime{}};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Create then open");
    TimelineWidget timeline(&controller);
    timeline.resize(1000, 500); timeline.setTool(Tool::Select); timeline.show();
    QApplication::processEvents();
    int opened = 0;
    QString openedClip;
    connect(&timeline, &TimelineWidget::openPianoRollRequested, &timeline,
            [&](const QString&, const QString& id) { ++opened; openedClip = id; });
    const QPointF point(timeline.secondsToX(0.5), timeline.laneCentreForTest(0));
    const auto doubleClick = [&] {
        QMouseEvent event(QEvent::MouseButtonDblClick, point, timeline.mapToGlobal(point),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&timeline, &event);
        QMouseEvent release(QEvent::MouseButtonRelease, point, timeline.mapToGlobal(point),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&timeline, &release);
    };
    doubleClick();
    const auto* created = controller.project().findTrack(track);
    const bool onlyCreate = created && created->clips.size() == 1 && opened == 0;
    doubleClick();
    const bool thenOpen = created && created->clips.size() == 1 && opened == 1 &&
                          openedClip.toStdString() == created->clips.front().id;
    std::printf("%s piano workflow: empty MIDI lane creates only, existing clip opens on double click\n",
                onlyCreate && thenOpen ? "PASS" : "FAIL");
    return onlyCreate && thenOpen;
}

bool PianoRollView::checkVelocityRampForTest(const QString& images) {
    daw::EngineController controller{daw::EngineController::TestRuntime{}};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Velocity ramp");
    const auto clipId = controller.addMidiClip(track, 0, 8);
    PianoRollView view(&controller);
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.resize(800, 400);
    view.setClip(QString::fromStdString(track), QString::fromStdString(clipId));
    view.show();
    QApplication::processEvents();
    view.m_showKeyboard = view.m_showVelocityLane = true;
    view.m_laneHeight = 110;
    view.m_laneParam = LaneParam::Velocity;
    view.m_pxPerBeat = 100;
    view.m_rowHeight = 16;
    view.m_scrollX = 0;
    view.m_scrollY = (127 - 66) * 16 - 150;
    view.m_tool = Tool::Draw;

    std::vector<daw::NoteModel> fixture(7);
    const double beats[] = {0.5, 1, 1.5, 2, 2, 3, 4};
    const int velocities[] = {90, 70, 50, 110, 80, 100, 60};
    for (std::size_t i = 0; i < fixture.size(); ++i) {
        fixture[i].id = "ramp-" + std::to_string(i);
        fixture[i].startBeats = beats[i];
        fixture[i].lengthBeats = 0.25;
        fixture[i].pitch = 60 + int(i);
        fixture[i].velocity = velocities[i];
        fixture[i].pan = float(i) * 0.1f - 0.3f;
    }
    controller.setClipNotes(track, clipId, fixture, "Prepare Velocity Ramp Check");
    view.invalidateSoundingPitchIndex();
    view.invalidateNotePaintIndex();
    const auto notes = [&] { return daw::midiNotes(*view.clip()); };
    const auto original = notes();
    view.m_selected = {QString::fromStdString(original.back().id)};
    view.m_primary = *view.m_selected.begin();
    const auto selection = view.m_selected;
    const auto primary = view.m_primary;
    const auto depth = controller.undoDepth();
    bool ok = true;
    const auto check = [&](bool pass, const char* label) {
        std::printf("%s velocity ramp: %s\n", pass ? "PASS" : "FAIL", label);
        ok &= pass;
    };
    const auto point = [&](double beat, int velocity) {
        return QPointF(view.beatsToX(beat), view.laneValueToY(velocity / 127.0));
    };
    const auto mouse = [&](QEvent::Type type, QPointF pos,
                           Qt::MouseButton button = Qt::RightButton) {
        QMouseEvent event(type, pos, view.mapToGlobal(pos),
                          type == QEvent::MouseMove ? Qt::NoButton : button,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : button,
                          Qt::NoModifier);
        QApplication::sendEvent(&view, &event);
    };
    const auto matches = [&](std::initializer_list<int> values) {
        if (values.size() != original.size()) return false;
        auto expected = original;
        auto value = values.begin();
        for (auto& note : expected) note.velocity = *value++;
        return notes() == expected;
    };
    mouse(QEvent::MouseButtonPress, point(1, 100) + QPointF(2, 0));
    mouse(QEvent::MouseMove, point(3, 20));
    check(matches({90, 100, 80, 60, 60, 20, 60}) &&
          view.m_selected == selection && view.m_primary == primary &&
          controller.undoDepth() == depth && view.m_auditionPitch < 0,
          "downward ramp follows note onset, includes chords and unselected notes, preserves other properties");
    if (!images.isEmpty()) {
        QDir().mkpath(images);
        view.grab().save(QDir(images).filePath("velocity-ramp.png"));
    }
    mouse(QEvent::MouseMove, point(2, 50));
    check(matches({90, 100, 75, 50, 50, 100, 60}),
          "shrinking the range restores notes outside the new line");
    mouse(QEvent::MouseMove, point(0.5, 40));
    check(matches({40, 100, 50, 110, 80, 100, 60}),
          "reversing left restores the previous span and draws in the other direction");
    mouse(QEvent::MouseMove, point(3, 20));
    mouse(QEvent::MouseButtonRelease, point(3, 40));
    const auto downward = notes();
    check(matches({90, 100, 85, 70, 70, 40, 60}) &&
          controller.undoDepth() == depth + 1 && !view.hasActivePointerGesture() &&
          view.m_laneOrig.empty(),
          "release applies its exact endpoint and commits one undo for the whole gesture");
    controller.undo();
    check(notes() == original, "undo restores all original velocities");
    controller.redo();
    check(notes() == downward, "redo restores the complete ramp");
    controller.undo();

    mouse(QEvent::MouseButtonPress, point(1, 20));
    mouse(QEvent::MouseButtonRelease, point(3, 100));
    check(matches({90, 20, 40, 60, 60, 100, 60}) &&
          controller.undoDepth() == depth + 1,
          "an upward ramp also works when the final move is coalesced into release");
    controller.undo();
    mouse(QEvent::MouseButtonPress, point(1, 127));
    mouse(QEvent::MouseMove, QPointF(view.beatsToX(3), view.laneTop() - 500));
    check(matches({90, 127, 127, 127, 127, 127, 60}),
          "dragging above the lane clamps velocity at 127");
    mouse(QEvent::MouseButtonRelease, QPointF(view.beatsToX(3), view.height() + 100));
    check(matches({90, 127, 96, 64, 64, 1, 60}),
          "dragging below the lane clamps at the existing minimum velocity of 1");
    controller.undo();

    mouse(QEvent::MouseButtonPress, point(1, 100));
    mouse(QEvent::MouseMove, point(2, 40));
    QEvent lostGrab(QEvent::UngrabMouse);
    QApplication::sendEvent(&view, &lostGrab);
    check(matches({90, 100, 70, 40, 40, 100, 60}) &&
          !view.hasActivePointerGesture() && controller.undoDepth() == depth + 1,
          "losing the mouse grab safely commits the last delivered endpoint");
    controller.undo();
    mouse(QEvent::MouseButtonPress, point(1, 100));
    mouse(QEvent::MouseMove, point(2, 40));
    view.setLaneParam(LaneParam::Pan);
    check(matches({90, 100, 70, 40, 40, 100, 60}) &&
          !view.hasActivePointerGesture() && controller.undoDepth() == depth + 1,
          "switching to pan ends the velocity gesture without changing pan");
    const auto panDepth = controller.undoDepth();
    mouse(QEvent::MouseButtonPress, point(1, 20));
    mouse(QEvent::MouseButtonRelease, point(3, 100));
    check(matches({90, 100, 70, 40, 40, 100, 60}) &&
          controller.undoDepth() == panDepth,
          "right-click behavior in the pan lane remains unchanged");
    view.setLaneParam(LaneParam::Velocity);
    controller.undo();

    mouse(QEvent::MouseButtonPress, point(5, 40));
    mouse(QEvent::MouseButtonRelease, point(6, 80));
    check(notes() == original && controller.undoDepth() == depth,
          "an empty span does not change notes or add history");
    const auto erasePoint = view.noteRect(original.front()).center();
    mouse(QEvent::MouseButtonPress, erasePoint);
    mouse(QEvent::MouseButtonRelease, erasePoint);
    auto erased = original;
    erased.erase(erased.begin());
    check(notes() == erased && controller.undoDepth() == depth + 1,
          "right-drag in the note grid still erases notes");
    controller.undo();
    view.m_selected = {QString::fromStdString(original[1].id),
                       QString::fromStdString(original[5].id)};
    view.m_primary = QString::fromStdString(original[1].id);
    mouse(QEvent::MouseButtonPress, point(1, 70), Qt::LeftButton);
    mouse(QEvent::MouseMove, point(1, 80), Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, point(1, 80), Qt::LeftButton);
    check(matches({90, 80, 50, 110, 80, 110, 60}) &&
          controller.undoDepth() == depth + 1,
          "left-drag still adjusts the selected group by a relative velocity offset");
    return ok;
}
