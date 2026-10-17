#include "TimelineWidget.hpp"
#include "MidiSequence.hpp"
#include "EngineController.hpp"
#include "ProjectSerializer.hpp"
#include "cloud/CloudDocumentProjection.hpp"
#include "UiConstants.hpp"
#include "graphics/SceneRecorder.hpp"
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
    check(clip()->midiView.sequence && clip()->midiView.pitch == 60 && clip()->midiView.stepBeats == .5 &&
          controller.undoDepth() == viewDepth, "C5 / 1/8 defaults, no musical undo for mode switch");
    auto testView = clip()->midiView;
    testView.stepBeats = .25;
    controller.setMidiClipView(track, id, testView);

    // The real GPU path must contain one title even across several time tiles.
    ui::graphics::SceneRecorder header(timeline.size(), 1);
    { QPainter p(&header); p.setClipRect(timeline.rect()); timeline.drawMidiClipHeaders(p); }
    QImage title;
    const auto findTitle = [&](const auto& self, const auto& meshes) -> void {
        for (const auto& mesh : meshes) {
            if (mesh.texture.width() > title.width()) title = mesh.texture;
            if (mesh.group) self(self, mesh.group->meshes);
        }
    };
    findTitle(findTitle, header.takeMeshes());
    ui::graphics::SceneRecorder frame(timeline.size(), 1);
    { QPainter p(&frame); timeline.drawStaticFrame(p, QRegion(timeline.rect())); }
    const auto countTitle = [&](const auto& self, const std::vector<ui::graphics::SceneMesh>& meshes) -> int {
        int count = 0;
        for (const auto& mesh : meshes) {
            if (!title.isNull() && mesh.texture == title) ++count;
            if (mesh.group) count += self(self, mesh.group->meshes);
        }
        return count;
    };
    check(!title.isNull() && frame.supported() && countTitle(countTitle, frame.takeMeshes()) == 1,
          "GPU time tiles render the MIDI clip title only once");
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

    const auto beforePaint = clip()->notes;
    const auto paintDepth = controller.undoDepth();
    send(QEvent::MouseButtonPress, stepPoint(5), Qt::LeftButton);
    send(QEvent::MouseMove, stepPoint(10), Qt::LeftButton);
    send(QEvent::MouseMove, stepPoint(6), Qt::LeftButton);
    check(clip()->notes.size() == beforePaint.size() + 6 && controller.undoDepth() == paintDepth,
          "fast paint fills skipped steps; reversing does not toggle them off");
    send(QEvent::MouseButtonRelease, stepPoint(6), Qt::NoButton);
    const auto paintedNotes = clip()->notes;
    check(controller.undoDepth() == paintDepth + 1, "whole paint gesture is one undo step");
    controller.undo(); check(clip()->notes == beforePaint, "paint undo restores original notes");
    controller.redo(); check(clip()->notes == paintedNotes, "paint redo restores all filled steps");
    controller.undo();

    send(QEvent::MouseButtonPress, stepPoint(0), Qt::LeftButton);
    send(QEvent::MouseMove, stepPoint(4), Qt::LeftButton);
    send(QEvent::MouseMove, stepPoint(0), Qt::LeftButton);
    check(clip()->notes.size() == 1 && clip()->notes.front().pitch == 62,
          "erase drag removes current pitch only and never creates notes in gaps");
    QKeyEvent cancelPaint(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &cancelPaint);
    check(clip()->notes == beforePaint && controller.undoDepth() == paintDepth && !timeline.m_sequenceEditing,
          "Escape cancels the whole erase gesture without history");
    send(QEvent::MouseButtonPress, stepPoint(10), Qt::LeftButton);
    send(QEvent::MouseMove, stepPoint(5), Qt::LeftButton);
    check(clip()->notes.size() == beforePaint.size() + 6, "paint fills skipped steps right to left");
    QApplication::sendEvent(&timeline, &cancelPaint);
    check(clip()->notes == beforePaint && controller.undoDepth() == paintDepth,
          "Escape cancels added steps without history");

    send(QEvent::MouseButtonPress, stepPoint(1), Qt::LeftButton);
    send(QEvent::MouseMove, stepPoint(4), Qt::LeftButton);
    check(clip()->notes.size() == beforePaint.size() + 3,
          "painting over existing steps keeps them and can share a cell with another pitch");
    QEvent endPaint(QEvent::WindowDeactivate); QApplication::sendEvent(&timeline, &endPaint);
    check(!timeline.m_sequenceEditing && controller.undoDepth() == paintDepth + 1,
          "lost capture commits a single paint operation");
    controller.undo();

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

    // Dense rows should stay a handful of GPU batches as the note count grows.
    // Also exercise narrow damage and fractional DPI, including stacked notes:
    // grouping by colour must not change their compositing order.
    auto dense = *clip();
    dense.id = "sequence-render-fixture";
    dense.durationSeconds = 16;
    dense.offsetSeconds = dense.contentOffsetBeats = 0;
    dense.midiView.stepBeats = .25;
    dense.notes.clear();
    for (int n = 0; n < 128; ++n) {
        daw::NoteModel note;
        note.startBeats = n * .25; note.lengthBeats = .125;
        note.pitch = dense.midiView.pitch; note.velocity = 100;
        dense.notes.push_back(note);
    }
    const double savedScale = timeline.m_pixelsPerSecond;
    timeline.m_pixelsPerSecond = 64;
    ui::graphics::SceneRecorder recorder(timeline.size());
    { QPainter painter(&recorder); painter.setClipRect(timeline.rect());
      timeline.drawMidiSequence(painter, *controller.project().findTrack(track), dense,
                                QRectF(0, 30, 1024, 50), 1); }
    const auto meshCount = [](const auto& self, const auto& meshes) -> std::size_t {
        std::size_t count = meshes.size();
        for (const auto& mesh : meshes) if (mesh.group) count += self(self, mesh.group->meshes);
        return count;
    };
    const auto meshes = recorder.takeMeshes();
    check(recorder.supported() && meshCount(meshCount, meshes) <= 16,
          "128 equal-velocity steps share bounded GPU batches");

    auto stacked = dense.notes[15];
    stacked.startBeats += .02; stacked.velocity = 50; stacked.muted = true;
    dense.notes.push_back(stacked);
    stacked.pitch += 2; dense.notes.push_back(stacked);
    bool repaintMatches = true;
    for (double dpr : {1., 1.5, 2.}) for (double scale : {4., 60., 64., 173.25, 320.}) {
        timeline.m_pixelsPerSecond = scale;
        const QRectF bounds(-2.25, 32.5, dense.durationSeconds * scale, 70);
        QImage image(QSize(int(timeline.width() * dpr), int(timeline.height() * dpr)), QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr); image.fill(Qt::transparent);
        const auto draw = [&](QImage& target, QRect damage) {
            QPainter painter(&target); painter.setRenderHint(QPainter::Antialiasing);
            painter.setClipRect(damage);
            timeline.drawMidiSequence(painter, *controller.project().findTrack(track), dense, bounds, 2);
        };
        draw(image, timeline.rect());
        QImage damaged = image;
        for (int x = 8; x < timeline.width(); x += 122) draw(damaged, QRect(x, 58, 18, 40));
        repaintMatches &= image == damaged;
        if (image != damaged) {
            std::fprintf(stderr, "Sequence repaint mismatch: dpr=%.1f scale=%.2f\n", dpr, scale);
            if (!images.isEmpty()) {
                const QString name = QString("/sequence-repaint-%1-%2").arg(dpr).arg(scale);
                image.save(images + name + "-full.png");
                damaged.save(images + name + "-partial.png");
            }
        }
    }
    check(repaintMatches, "batched steps preserve full/partial painting at fractional DPI and all zoom levels");

    timeline.m_pixelsPerSecond = 60;
    dense.notes.clear();
    const QRectF distantBody(0, 30, 960, 70);
    QImage distant(timeline.size(), QImage::Format_ARGB32_Premultiplied); distant.fill(Qt::transparent);
    { QPainter painter(&distant);
      timeline.drawMidiSequence(painter, *controller.project().findTrack(track), dense, distantBody, 3); }
    const QRect interior = ui::sequence::Layout(distantBody, true, timeline.width()).steps.toAlignedRect().adjusted(5, 1, -5, -1);
    const QRgb well = distant.pixel(interior.topLeft());
    bool unobstructed = true;
    for (int y = interior.top(); y <= interior.bottom(); ++y)
        for (int x = interior.left(); x <= interior.right(); ++x) unobstructed &= distant.pixel(x, y) == well;
    check(unobstructed, "distant sequence stays unobstructed by a zoom-to-edit message");
    timeline.m_pixelsPerSecond = savedScale;

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

    // Exercise the arrangement's real copy/paste keys and a single click on
    // the destination Pattern, with the playhead deliberately elsewhere.
    {
        daw::EngineController patterns{};
        if (!patterns.initialize(48000, 512, false).isOk()) return false;
        const auto pattern = patterns.addPattern("Clipboard Pattern");
        const auto owner = patterns.project().findTrack(pattern)->clips.front().id;
        const auto a = patterns.addTrack(daw::TrackKind::Midi, "Kick");
        const auto b = patterns.addTrack(daw::TrackKind::Midi, "Snare");
        patterns.moveTrackToFolder(a, pattern);
        patterns.moveTrackToFolder(b, pattern);
        const auto first = patterns.addMidiClip(a, .5, .5);
        const auto second = patterns.addMidiClip(b, 1.0, .5);
        patterns.addNote(a, first, 60, 0, .5);
        patterns.addNote(b, second, 62, 0, .5);
        const auto destination = patterns.duplicateClipAt(pattern, owner, 4.0);
        TimelineWidget arrangement(&patterns);
        arrangement.setAttribute(Qt::WA_DontShowOnScreen);
        arrangement.resize(1040, 400);
        arrangement.m_pixelsPerSecond = 120;
        arrangement.m_scrollSeconds = 0;
        arrangement.setTool(Tool::Select);
        arrangement.show();
        QApplication::processEvents();
        const auto q = [](const std::string& value) { return QString::fromStdString(value); };
        const auto find = [&](const std::string& lane, const std::string& clipId) {
            return arrangement.findClipModel(q(lane), q(clipId));
        };
        const auto key = [&](int code) {
            QKeyEvent press(QEvent::KeyPress, code, Qt::ControlModifier);
            QApplication::sendEvent(&arrangement, &press);
            QKeyEvent release(QEvent::KeyRelease, code, Qt::ControlModifier);
            QApplication::sendEvent(&arrangement, &release);
        };
        const auto selectDestination = [&] {
            const QPointF at = arrangement.clipRect(arrangement.laneForTrackId(q(pattern)),
                                                    *find(pattern, destination)).center();
            QMouseEvent press(QEvent::MouseButtonPress, at, arrangement.mapToGlobal(at),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&arrangement, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, at, arrangement.mapToGlobal(at),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&arrangement, &release);
        };
        for (const bool multiple : {false, true}) {
            QVector<ui::ClipSel> selected{{q(a), q(first)}};
            if (multiple) selected.push_back({q(b), q(second)});
            arrangement.selectClips(selected);
            key(Qt::Key_C);
            selectDestination();
            check(arrangement.m_selectedClipId == q(destination), "one click selects the destination Pattern");
            patterns.seekSeconds(20.0);
            const auto undo = patterns.undoDepth();
            key(Qt::Key_V);
            const auto pasted = arrangement.m_selection;
            bool placed = pasted.size() == (multiple ? 2 : 1);
            for (const auto& ref : pasted) {
                const auto* inserted = arrangement.findClipModel(ref.trackId, ref.clipId);
                const bool kick = ref.trackId == q(a);
                placed &= inserted && inserted->kind == daw::ClipKind::Midi &&
                    inserted->patternClipId == destination && inserted->startSeconds == (kick ? 4.5 : 5.0) &&
                    inserted->notes.size() == 1 && inserted->notes.front().pitch == (kick ? 60 : 62) &&
                    inserted->contentId != find(kick ? a : b, kick ? first : second)->contentId;
            }
            check(placed && patterns.undoDepth() == undo + 1 &&
                      find(pattern, owner)->durationSeconds == 2.0 && find(pattern, destination)->durationSeconds == 2.0,
                  multiple ? "Ctrl+V preserves lanes and Pattern-relative offsets for multiple MIDI clips"
                           : "Ctrl+V places one MIDI clip in the clicked Pattern independently of the playhead");
            patterns.undo();
            bool removed = true;
            for (const auto& ref : pasted) removed &= !arrangement.findClipModel(ref.trackId, ref.clipId);
            check(removed, "one undo removes the complete Pattern paste");
            patterns.redo();
            bool restored = true;
            for (const auto& ref : pasted) {
                const auto* inserted = arrangement.findClipModel(ref.trackId, ref.clipId);
                restored &= inserted && inserted->patternClipId == destination;
            }
            check(restored, "redo retains the destination Pattern ownership");
            patterns.undo();
        }
        // With no Pattern selected, ordinary paste still follows the playhead.
        arrangement.selectClips({{q(a), q(first)}});
        key(Qt::Key_C);
        arrangement.selectClips({});
        patterns.seekSeconds(1.0);
        key(Qt::Key_V);
        const auto* ordinary = arrangement.m_selection.size() == 1
            ? arrangement.findClipModel(arrangement.m_selection.front().trackId,
                                        arrangement.m_selection.front().clipId) : nullptr;
        check(ordinary && ordinary->patternClipId == owner && ordinary->startSeconds == 1.0,
              "ordinary arrangement paste still follows the playhead");
    }
    return ok;
}
