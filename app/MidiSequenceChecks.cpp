#include "TimelineWidget.hpp"
#include "MidiSequence.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "cloud/CloudDocumentProjection.hpp"
#include "UiConstants.hpp"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <cstdio>

bool TimelineWidget::checkMidiSequenceForTest(const QString& images) {
    daw::EngineController controller{};
    if (!controller.initialize(48000, 512, false).isOk()) return false;
    controller.setTempo(120);
    const auto track = controller.addTrack(daw::TrackKind::Midi, "Drum sequence");
    const auto id = controller.addMidiClip(track, 0, 2);
    controller.setClipName(track, id, "Drums · C5 / D5");
    TimelineWidget timeline(&controller);
    timeline.setAttribute(Qt::WA_DontShowOnScreen);
    timeline.resize(1040, 280);
    timeline.m_pixelsPerSecond = 320;
    timeline.m_scrollSeconds = 0;
    timeline.setTool(Tool::Select);
    timeline.show();
    QApplication::processEvents();
    const auto clip = [&] { return timeline.findClipModel(QString::fromStdString(track), QString::fromStdString(id)); };
    const auto layout = [&] { return ui::sequence::Layout(timeline.clipRect(0, *clip()), clip()->midiView.sequence, timeline.width() - ui::kTimelineScrollExtent); };
    const auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, timeline.mapToGlobal(at),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, Qt::NoModifier);
        QApplication::sendEvent(&timeline, &event);
    };
    const auto click = [&](QPointF at) {
        send(QEvent::MouseButtonPress, at, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, at, Qt::NoButton);
    };
    const auto stepPoint = [&](int step) {
        return QPointF(timeline.secondsToX(clip()->startSeconds + daw::beatsToSeconds(
            (step + .5) * clip()->midiView.stepBeats, controller.tempo())), layout().steps.center().y() + 3);
    };
    bool ok = true;
    const auto check = [&](bool pass, const char* label) {
        std::printf("%s sequence: %s\n", pass ? "PASS" : "FAIL", label); ok &= pass;
    };
    const auto save = [&](const char* name) {
        if (images.isEmpty()) return;
        QDir().mkpath(images); QApplication::processEvents();
        check(timeline.grab().save(QDir(images).filePath(QString::fromLatin1(name))), "screenshot saved");
    };
    const auto viewDepth = controller.undoDepth();
    click(layout().sequence.center());
    check(clip()->midiView.sequence && clip()->midiView.pitch == 60 && clip()->midiView.stepBeats == .25 &&
          controller.undoDepth() == viewDepth, "C5 / 1/16 defaults, no musical undo for mode switch");
    click(stepPoint(0)); click(stepPoint(4));
    check(clip()->notes.size() == 2 && clip()->notes[0].pitch == 60 && clip()->notes[0].lengthBeats == .125 &&
          clip()->notes[1].startBeats == 1, "clicks insert short C5 notes on the step grid");
    if (clip()->notes.size() != 2) return false;
    ClipHit hit; timeline.hitTestClip(stepPoint(0).toPoint(), hit);
    const auto setPitch = [&](int pitch) {
        QMenu menu; timeline.populateSequenceMenu(menu, hit);
        auto* action = menu.findChild<QAction*>(QStringLiteral("SequencePitch%1").arg(pitch));
        check(action != nullptr, "native pitch selector");
        if (action) action->trigger();
    };
    setPitch(62); click(stepPoint(2)); setPitch(60);
    check(clip()->notes.size() == 3 && clip()->notes.back().pitch == 62 && clip()->notes[0].pitch == 60,
          "D5 insertion leaves C5 notes unchanged");
    SequenceHit ghost; timeline.hitTestSequence(stepPoint(2).toPoint(), ghost);
    check(ghost.part == SequencePart::Step && ghost.noteId.empty(), "ghost pitch leaves an empty step for current pitch");
    click(stepPoint(2)); check(clip()->notes.size() == 4, "C5 can share a step with D5");
    click(stepPoint(2)); check(clip()->notes.size() == 3 && clip()->notes.back().pitch == 62, "toggle removes only current pitch");
    controller.undo(); check(clip()->notes.size() == 4, "step removal undo"); controller.redo();

    const auto velocityPoint = [&] {
        const double pixels = daw::beatsToSeconds(clip()->midiView.stepBeats, controller.tempo()) * timeline.m_pixelsPerSecond;
        const auto r = ui::sequence::cellRect(layout(), timeline.clipRect(0, *clip()).left(), pixels, clip()->notes.front().velocity);
        return QPointF(r.center().x(), r.top() + 1);
    };
    const auto depth = controller.undoDepth(); const auto top = velocityPoint();
    timeline.updateCursor(top.toPoint());
    check(timeline.cursor().shape() == Qt::SizeVerCursor, "top edge shows vertical cursor");
    send(QEvent::MouseButtonPress, top, Qt::LeftButton);
    send(QEvent::MouseMove, top + QPointF(0, 6), Qt::LeftButton);
    const int quiet = clip()->notes.front().velocity;
    check(quiet < 100 && controller.undoDepth() == depth, "live velocity, no per-move history");
    send(QEvent::MouseMove, top + QPointF(0, 3), Qt::LeftButton);
    const int softer = clip()->notes.front().velocity;
    send(QEvent::MouseButtonRelease, top + QPointF(0, 3), Qt::NoButton);
    check(softer > quiet && softer < 100 && controller.undoDepth() == depth + 1, "immediate reversal, one undo per drag");
    controller.undo(); check(clip()->notes.front().velocity == 100, "velocity undo");
    controller.redo(); check(clip()->notes.front().velocity == softer, "velocity redo");
    const auto escapeDepth = controller.undoDepth(); const auto newTop = velocityPoint();
    send(QEvent::MouseButtonPress, newTop, Qt::LeftButton);
    send(QEvent::MouseMove, newTop + QPointF(0, 8), Qt::LeftButton);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier); QApplication::sendEvent(&timeline, &escape);
    check(!timeline.m_sequenceEditing && clip()->notes.front().velocity == softer && controller.undoDepth() == escapeDepth,
          "Escape restores original value without history entry");
    const auto clampTop = velocityPoint();
    send(QEvent::MouseButtonPress, clampTop, Qt::LeftButton);
    send(QEvent::MouseMove, clampTop + QPointF(0, 200), Qt::LeftButton);
    check(clip()->notes.front().velocity == 1, "velocity stays in the audible MIDI range");
    send(QEvent::MouseMove, clampTop + QPointF(0, 199), Qt::LeftButton);
    check(clip()->notes.front().velocity > 1, "reversal at minimum has no dead travel");
    QApplication::sendEvent(&timeline, &escape);
    const auto interruptTop = velocityPoint();
    send(QEvent::MouseButtonPress, interruptTop, Qt::LeftButton);
    send(QEvent::MouseMove, interruptTop + QPointF(0, -3), Qt::LeftButton);
    QEvent deactivate(QEvent::WindowDeactivate); QApplication::sendEvent(&timeline, &deactivate);
    check(!timeline.m_sequenceEditing && controller.undoDepth() == escapeDepth + 1, "lost capture closes edit transaction");

    int opened = 0;
    connect(&timeline, &TimelineWidget::openPianoRollRequested, &timeline, [&](const QString&, const QString&) { ++opened; });
    const auto empty = stepPoint(7); click(empty); const auto count = clip()->notes.size();
    send(QEvent::MouseButtonDblClick, empty, Qt::LeftButton); send(QEvent::MouseButtonRelease, empty, Qt::NoButton);
    check(clip()->notes.size() == count && opened == 0, "double click in steps preserves new note");
    send(QEvent::MouseButtonDblClick, layout().name.center(), Qt::LeftButton);
    send(QEvent::MouseButtonRelease, layout().name.center(), Qt::NoButton);
    check(opened == 1, "header double click opens piano roll");
    const auto notes = clip()->notes;
    click(layout().midi.center());
    check(!clip()->midiView.sequence && clip()->notes == notes && layout().pitch.isEmpty(), "MIDI view preserves all notes and hides controls");
    save("midi-view.png"); click(layout().sequence.center());
    { QMenu menu; timeline.populateSequenceMenu(menu, hit);
      if (auto* grid = menu.findChild<QMenu*>("SequenceGridMenu"))
          for (auto* action : grid->actions()) if (action->text() == "1/8") action->trigger(); }
    check(clip()->midiView.stepBeats == .5 && clip()->notes == notes, "density changes do not quantize notes");
    auto view = clip()->midiView; view.stepBeats = .25; controller.setMidiClipView(track, id, view);
    const auto lengthDepth = controller.undoDepth();
    QTimer::singleShot(0, &timeline, [&] {
        if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
            dialog->setIntValue(24); dialog->accept();
        }
    });
    timeline.showSequenceControl(hit, SequencePart::Length, {});
    check(std::abs(clip()->durationSeconds - 3) < 1e-8 && clip()->notes == notes &&
          controller.undoDepth() == lengthDepth + 1, "length control changes step count without touching notes");
    controller.undo();
    check(std::abs(clip()->durationSeconds - 2) < 1e-8, "sequence length undo");
    controller.addNote(track, id, 60, 2, .125, 35); controller.addNote(track, id, 60, 3, .125, 115);
    controller.addNote(track, id, 62, 2.5, .125, 80);
    timeline.update(); save("sequence-c5.png"); setPitch(62); save("sequence-d5.png"); setPitch(60);
    controller.addNote(track, id, 60, 3.6, .125, 90);
    const auto body = timeline.clipRect(0, *clip());
    QImage full(timeline.size(), QImage::Format_ARGB32_Premultiplied); full.fill(Qt::transparent);
    { QPainter painter(&full); timeline.drawMidiSequence(painter, *controller.project().findTrack(track), *clip(), body,
                                                        controller.midiNotesRevision(track)); }
    QImage partial = full;
    const QRect stripe(timeline.secondsToX(daw::beatsToSeconds(3.5, controller.tempo())), int(body.top()) + 25, 5, int(body.height()) - 26);
    { QPainter painter(&partial); painter.setClipRect(stripe); painter.fillRect(stripe, Qt::red);
      timeline.drawMidiSequence(painter, *controller.project().findTrack(track), *clip(), body, controller.midiNotesRevision(track)); }
    if (full != partial && !images.isEmpty()) {
        full.save(QDir(images).filePath("repaint-full.png"));
        partial.save(QDir(images).filePath("repaint-partial.png"));
    }
    check(full == partial, "partial repaint matches a full frame for off-grid notes");
    std::string bytes; daw::ProjectModel restored;
    const bool roundtrip = daw::ProjectSerializer::serializeDocument(controller.project(), bytes).isOk() &&
        daw::ProjectSerializer::deserializeDocument(restored, bytes).isOk();
    check(roundtrip && restored.findTrack(track) && restored.findTrack(track)->clips.front().midiView == clip()->midiView &&
        restored.findTrack(track)->clips.front().notes == clip()->notes, "document roundtrip retains view and all MIDI data");
    const auto shared = daw::cloud::projectForCloudSnapshotV1(controller.project());
    check(shared.valid() && shared.document.findTrack(track)->clips.front().midiView == daw::MidiClipView{} &&
        shared.document.findTrack(track)->clips.front().notes == clip()->notes && clip()->midiView.sequence,
        "cloud snapshot carries notes while view preferences stay local");
    const auto copyId = controller.duplicateClip(track, id);
    const auto* copy = timeline.findClipModel(QString::fromStdString(track), QString::fromStdString(copyId));
    const bool samePhrase = copy && copy->notes.size() == clip()->notes.size() &&
        std::equal(copy->notes.begin(), copy->notes.end(), clip()->notes.begin(), [](auto a, auto b) {
            a.id.clear(); b.id.clear(); return a == b;
        });
    check(samePhrase && copy->midiView == clip()->midiView, "duplicate retains view settings and musical phrase");
    controller.setMidiClipView(track, copyId, {false, 72, .5});
    check(clip()->midiView.sequence && clip()->midiView.pitch == 60, "duplicate can choose its own view");
    controller.undo();
    controller.beginClipTrimEdit(track, id); controller.setClipTrim(track, id, .25, .25, 1.75); controller.endClipTrimEdit("Trim Clip");
    timeline.update(); const auto before = clip()->notes.size(); click(stepPoint(1));
    check(clip()->notes.size() == before + 1 && std::abs(clip()->notes.back().startBeats - .75) < 1e-8,
        "insertion respects trimmed source offset");
    controller.undo();
    const double beatOffset = clip()->contentOffsetBeats;
    // Canonical content can carry an authoritative offset in beats while a
    // legacy seconds field is still present. Cover both representations.
    const_cast<daw::ClipModel*>(clip())->contentOffsetBeats = 1;
    click(stepPoint(1));
    check(clip()->notes.size() == before + 1 && std::abs(clip()->notes.back().startBeats - 1.25) < 1e-8,
        "canonical beat offset takes precedence over legacy seconds");
    controller.undo();
    const_cast<daw::ClipModel*>(clip())->contentOffsetBeats = beatOffset;
    controller.undo();
    timeline.m_scrollSeconds = .5; timeline.update(); save("sequence-scrolled.png");
    check(layout().pitch.left() >= 0 && layout().sequence.right() <= timeline.width(), "scrolled header controls stay visible");
    timeline.m_scrollSeconds = 0; timeline.m_pixelsPerSecond = 60; timeline.update(); save("sequence-narrow.png");
    SequenceHit narrow;
    check(!timeline.hitTestSequence(stepPoint(3).toPoint(), narrow) ||
        (narrow.part != SequencePart::Step && narrow.part != SequencePart::Velocity), "distant zoom has no invisible step targets");
    controller.setTrackHeight(track, 24); timeline.update();
    check(layout().sequence.isEmpty(), "collapsed lane has no invisible header buttons");
    return ok;
}
