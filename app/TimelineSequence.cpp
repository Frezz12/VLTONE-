#include "TimelineWidget.hpp"
#include "MidiSequence.hpp"
#include "EngineController.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"
#include <QActionGroup>
#include <QCursor>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

namespace seq = ui::sequence;
namespace {
bool sequenceAvailable(const daw::ClipModel& clip) {
    return clip.kind == daw::ClipKind::Midi && !daw::isLayered(clip);
}
double sourceOffset(const daw::ClipModel& clip, double tempo) {
    return clip.contentOffsetBeats != 0.0 ? clip.contentOffsetBeats
        : daw::secondsToBeats(clip.offsetSeconds, tempo);
}
double endBeat(const daw::ClipModel& clip, double tempo) {
    return sourceOffset(clip, tempo) + daw::secondsToBeats(clip.durationSeconds, tempo);
}
}

void TimelineWidget::applyMidiView(const ClipHit& hit, daw::MidiClipView view) {
    finishSequence();
    m_controller->setMidiClipView(hit.trackId.toStdString(), hit.clipId.toStdString(), view);
    emit projectEdited();
    update();
}

void TimelineWidget::drawMidiClipHeader(QPainter& p, const daw::ClipModel& clip,
                                         const QRectF& body) {
    const bool sequenced = clip.midiView.sequence && sequenceAvailable(clip);
    const seq::Layout layout(body, sequenced, width() - ui::kTimelineScrollExtent);
    if (layout.header.isEmpty()) return;
    p.save();
    p.setClipRect(body, Qt::IntersectClip);
    const QColor text(255, 255, 255, 235);
    p.setFont(ui::transportControlFont(10));
    const auto chip = [&](QRectF rect, const QString& value) {
        if (rect.isEmpty()) return;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 55));
        p.drawRoundedRect(rect.adjusted(0, 3, 0, -3), 3, 3);
        p.setPen(text);
        p.drawText(rect.adjusted(5, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, value);
        icons::paint(p, icons::Glyph::Chevron,
                     QRectF(rect.right() - 11, rect.center().y() - 4, 8, 8), text);
    };
    chip(layout.pitch, seq::pitchName(clip.midiView.pitch));
    chip(layout.division, seq::divisionName(clip.midiView.stepBeats));
    QRectF name = layout.name;
    if (clip.muted && name.width() >= 16) {
        const QPointF center(name.left() + 6, name.center().y());
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(text, 1.2));
        p.drawEllipse(center, 4, 4);
        p.drawLine(center + QPointF(-3, 3), center + QPointF(3, -3));
        name.adjust(16, 0, 0, 0);
    }
    p.setPen(text);
    if (name.width() >= 14)
        p.drawText(name, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(p.font()).elidedText(linkedName(clip), Qt::ElideRight, int(name.width())));
    const auto mode = [&](QRectF rect, icons::Glyph glyph, bool active, bool enabled) {
        if (rect.isEmpty()) return;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, active ? 105 : 30));
        p.drawRoundedRect(rect.adjusted(1, 2, -1, -2), 3, 3);
        icons::paint(p, glyph, rect.adjusted(5, 5, -5, -5),
                     QColor(255, 255, 255, enabled ? (active ? 255 : 175) : 70));
        if (active) p.fillRect(QRectF(rect.left() + 6, rect.bottom() - 3, rect.width() - 12, 1), text);
    };
    mode(layout.midi, icons::Glyph::MidiKeys, !sequenced, true);
    mode(layout.sequence, icons::Glyph::GridDivision, sequenced, sequenceAvailable(clip));
    p.restore();
}

void TimelineWidget::drawMidiSequence(QPainter& p, const daw::TrackModel& track,
                                      const daw::ClipModel& clip, const QRectF& body,
                                      std::uint64_t revision) {
    const seq::Layout layout(body, true, width() - ui::kTimelineScrollExtent);
    if (layout.steps.height() < 3) return;
    const double tempo = m_controller->tempo();
    const double step = clip.midiView.stepBeats;
    const double pixels = daw::beatsToSeconds(step, tempo) * m_pixelsPerSecond;
    const double offset = sourceOffset(clip, tempo);
    const double end = endBeat(clip, tempo);
    const auto xAt = [&](double beat) {
        return body.left() + daw::beatsToSeconds(beat - offset, tempo) * m_pixelsPerSecond;
    };
    p.save();
    p.setClipRect(body.adjusted(1, 24, -1, -1), Qt::IntersectClip);
    const auto dirty = p.clipBoundingRect();
    const double first = std::max(offset, offset + daw::secondsToBeats((dirty.left() - body.left()) / m_pixelsPerSecond, tempo));
    const double last = std::min(end, offset + daw::secondsToBeats((dirty.right() - body.left()) / m_pixelsPerSecond, tempo));
    const QColor color = colorFromRgb(track.color);
    QColor background = mixColors(th().well(), color, th().dark ? .14 : .09);
    // Color interpolation can round an opaque alpha down to 254. This well
    // replaces the clip background, so repainting it must fully cover it.
    background.setAlpha(255);
    p.fillRect(QRectF(body.left() + 1, body.top() + 24, body.width() - 2, body.height() - 25),
               background);

    // At a distant zoom show occupancy without creating thousands of tiny
    // targets. The same zoom threshold is used for hit testing.
    if (pixels >= 8) {
        p.setPen(Qt::NoPen);
        for (double beat = seq::stepAt(first, step); beat < last - 1e-8; beat += step) {
            const auto quarter = std::llround(std::floor(beat + 1e-8));
            p.setBrush(mixColors(th().well(), color, quarter % 2 ? .28 : .18));
            p.drawRoundedRect(seq::cellRect(layout, xAt(beat), pixels), 2, 2);
            if (pixels >= 22 && std::abs(beat - std::round(beat)) < 1e-8 && !layout.footer.isEmpty()) {
                p.setFont(ui::transportControlFont(9));
                p.setPen(th().textSecondary);
                p.drawText(QRectF(xAt(beat) + 3, layout.footer.top(), pixels, layout.footer.height()),
                           Qt::AlignVCenter | Qt::AlignLeft, QString::number(quarter + 1));
                p.setPen(Qt::NoPen);
            }
        }
    }
    const auto& notes = clip.notes;
    const auto& index = midiPreviewIndex(clip.id, clip.id, notes, revision);
    index.forEachVisible(notes, seq::stepAt(first, step), std::min(end, seq::stepAt(last, step) + step),
        [&](const daw::NoteModel& note, std::size_t) {
        if (note.startBeats < offset - 1e-8 || note.startBeats >= end - 1e-8) return;
        const double beat = seq::stepAt(note.startBeats, step);
        // Keep off-grid note timing visible. Changing the drawing grid never
        // changes a note's onset or duration in the document.
        const double x = xAt(beat);
        const QRectF cell = seq::cellRect(layout, x, pixels, note.velocity);
        if (cell.right() < dirty.left() || cell.left() > dirty.right()) return;
        p.setPen(Qt::NoPen);
        if (note.pitch != clip.midiView.pitch) {
            p.setBrush(mixColors(th().well(), color, .55));
            p.drawRoundedRect(QRectF(cell.left(), layout.steps.top() - 5, cell.width(), 3), 1, 1);
            return;
        }
        const QColor fill = mixColors(mixColors(th().well(), color, .35), color, note.velocity / 127.0);
        p.setBrush(note.muted ? mixColors(th().well(), fill, .3) : fill);
        p.setPen(QPen(mixColors(fill, th().textPrimary, .20), 1, note.muted ? Qt::DashLine : Qt::SolidLine));
        p.drawRoundedRect(cell, 2, 2);
        p.setPen(Qt::NoPen);
        p.fillRect(QRectF(cell.left() + 1, cell.top(), std::max(1.0, cell.width() - 2), 2),
                   mixColors(fill, th().textPrimary, .48));
        if (std::abs(note.startBeats - beat) > 1e-6 && pixels >= 8) {
            const double onset = xAt(note.startBeats);
            p.fillRect(QRectF(onset, cell.top() + 3, 1, std::max(1.0, cell.height() - 5)), th().textPrimary);
        }
    });
    if (!layout.footer.isEmpty()) {
        const double count = daw::secondsToBeats(clip.durationSeconds, tempo) / step;
        const QString label = tr("Steps: %1").arg(QString::number(count, 'g', 4));
        p.setFont(ui::transportControlFont(9));
        const double labelWidth = QFontMetrics(p.font()).horizontalAdvance(label) + 10;
        const QRectF badge(layout.footer.right() - labelWidth, layout.footer.top(), labelWidth, layout.footer.height());
        if (badge.left() >= layout.footer.left()) {
            p.fillRect(badge, background);
            p.setPen(th().textSecondary);
            p.drawText(badge, Qt::AlignVCenter | Qt::AlignRight, label);
        }
    }
    if (pixels < 8 && layout.header.width() > 120) {
        p.setPen(th().textPrimary);
        p.setFont(ui::transportControlFont(10));
        p.drawText(layout.steps.intersected(QRectF(0, 0, width(), height())), Qt::AlignCenter, tr("Zoom in to edit steps"));
    }
    p.restore();
}

bool TimelineWidget::hitTestSequence(const QPoint& pos, SequenceHit& hit) {
    if (!hitTestClip(pos, hit.clip) || hit.clip.kind != daw::ClipKind::Midi) return false;
    const auto* clip = findClipModel(hit.clip.trackId, hit.clip.clipId);
    if (!clip) return false;
    const QRectF body = clipRect(laneAt(pos.y()), *clip);
    if (!body.contains(pos)) return false;
    const bool sequenced = clip->midiView.sequence && sequenceAvailable(*clip);
    const seq::Layout layout(body, sequenced, width() - ui::kTimelineScrollExtent);
    if (layout.midi.contains(pos)) hit.part = SequencePart::Midi;
    else if (layout.sequence.contains(pos)) hit.part = SequencePart::Sequence;
    else if (layout.pitch.contains(pos)) hit.part = SequencePart::Pitch;
    else if (layout.division.contains(pos)) hit.part = SequencePart::Division;
    else if (sequenced && hit.clip.edge == Edge::None && layout.footer.contains(pos)) hit.part = SequencePart::Length;
    else if (sequenced && hit.clip.edge == Edge::None && layout.steps.adjusted(0, -3, 0, 0).contains(pos)) {
        const double step = clip->midiView.stepBeats;
        const double pixels = daw::beatsToSeconds(step, m_controller->tempo()) * m_pixelsPerSecond;
        if (pixels < 8 || layout.steps.height() < 3) return false;
        const double offset = sourceOffset(*clip, m_controller->tempo());
        const double beat = offset + daw::secondsToBeats((pos.x() - body.left()) / m_pixelsPerSecond, m_controller->tempo());
        hit.beat = std::max(offset, seq::stepAt(beat, step));
        if (hit.beat >= endBeat(*clip, m_controller->tempo()) - 1e-8) return false;
        hit.part = SequencePart::Step;
        const double cellStart = seq::stepAt(beat, step);
        const auto& index = midiPreviewIndex(clip->id, clip->id, clip->notes,
            m_controller->midiNotesRevision(hit.clip.trackId.toStdString()));
        index.forEachVisible(clip->notes, hit.beat, cellStart + step, [&](const daw::NoteModel& note, std::size_t) {
            if (note.pitch != clip->midiView.pitch || note.startBeats < hit.beat - 1e-8 ||
                note.startBeats >= endBeat(*clip, m_controller->tempo()) - 1e-8) return;
            if (!hit.noteId.empty() && note.velocity < hit.velocity) return;
            hit.noteId = note.id;
            hit.velocity = note.velocity;
        });
        hit.height = std::max(1.0, layout.steps.height() - 5);
        if (!hit.noteId.empty()) {
            const double x = body.left() + daw::beatsToSeconds(cellStart - offset, m_controller->tempo()) * m_pixelsPerSecond;
            const auto cell = seq::cellRect(layout, x, pixels, hit.velocity);
            if (std::abs(pos.y() - cell.top()) <= 3) hit.part = SequencePart::Velocity;
        }
    }
    return hit.part != SequencePart::None;
}

QString TimelineWidget::sequenceToolTip(const SequenceHit& hit) const {
    switch (hit.part) {
    case SequencePart::Midi: return tr("MIDI view · double-click the header to open piano roll");
    case SequencePart::Sequence: return tr("Sequence view · edit the same notes as steps");
    case SequencePart::Pitch: return tr("Note for new steps · existing notes keep their pitch");
    case SequencePart::Division: return tr("Step grid · independent of the arrangement grid");
    case SequencePart::Length: return tr("Sequence length · drag the clip edge or click to enter steps");
    case SequencePart::Velocity: return tr("Velocity %1 · drag the top edge up or down").arg(hit.velocity);
    case SequencePart::Step: return hit.noteId.empty() ? tr("Click to add a note") : tr("Click to remove · drag the top edge to change velocity");
    default: return {};
    }
}

bool TimelineWidget::pressSequence(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || (tool() != Tool::Select && tool() != Tool::Draw) ||
        (event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::MetaModifier))) return false;
    SequenceHit hit;
    if (!hitTestSequence(event->position().toPoint(), hit)) return false;
    const auto* clip = findClipModel(hit.clip.trackId, hit.clip.clipId);
    if (!clip) return false;
    // The second press of a double click in the step row is consumed. Opening
    // the roll belongs to the header/menu and must never remove a fresh step.
    if (event->type() == QEvent::MouseButtonDblClick) { event->accept(); return true; }
    m_selection = {ClipRef{hit.clip.trackId, hit.clip.clipId}};
    m_selectedClipId = hit.clip.clipId;
    publishSelection();
    emit clipSelected(hit.clip.trackId, hit.clip.clipId);
    clip = findClipModel(hit.clip.trackId, hit.clip.clipId);
    if (!clip) { event->accept(); return true; }
    if (hit.part == SequencePart::Midi || hit.part == SequencePart::Sequence) {
        if (hit.part == SequencePart::Midi || sequenceAvailable(*clip)) {
            auto view = clip->midiView;
            view.sequence = hit.part == SequencePart::Sequence;
            applyMidiView(hit.clip, view);
        }
    } else if (hit.part == SequencePart::Pitch || hit.part == SequencePart::Division || hit.part == SequencePart::Length) {
        showSequenceControl(hit.clip, hit.part, event->globalPosition().toPoint());
    } else if (m_controller->sharedEditingAllowed()) {
        const auto track = hit.clip.trackId.toStdString(), id = hit.clip.clipId.toStdString();
        if (hit.part == SequencePart::Velocity) {
            m_sequenceDrag = hit;
            m_sequenceStartY = event->position().y();
            m_sequenceVelocity = hit.velocity;
            m_sequenceEditing = true;
            m_controller->beginNoteEdit(track, id);
            setCursor(Qt::SizeVerCursor);
        } else if (!hit.noteId.empty()) {
            m_controller->removeNote(track, id, hit.noteId);
            emit projectEdited();
        } else {
            const double length = std::min({clip->midiView.stepBeats * .5, .125,
                                           endBeat(*clip, m_controller->tempo()) - hit.beat});
            m_controller->addNote(track, id, clip->midiView.pitch, hit.beat, length, 100);
            emit projectEdited();
        }
    }
    update();
    event->accept();
    return true;
}

void TimelineWidget::moveSequence(QMouseEvent* event) {
    // Incremental deltas keep reversal immediate after hitting 1 or 127;
    // there is no invisible overshoot for the pointer to travel back through.
    m_sequenceVelocity = std::clamp(m_sequenceVelocity +
        (m_sequenceStartY - event->position().y()) * 127.0 / m_sequenceDrag.height, 1.0, 127.0);
    m_sequenceStartY = event->position().y();
    const int next = int(std::lround(m_sequenceVelocity));
    m_controller->setNoteVelocity(m_sequenceDrag.clip.trackId.toStdString(),
        m_sequenceDrag.clip.clipId.toStdString(), m_sequenceDrag.noteId, next);
    setCursor(Qt::SizeVerCursor);
    QToolTip::showText(event->globalPosition().toPoint(), tr("Velocity %1").arg(next), this);
    invalidateTrack(m_sequenceDrag.clip.trackId);
    event->accept();
}

void TimelineWidget::finishSequence(bool cancel) {
    if (!m_sequenceEditing) return;
    m_sequenceEditing = false;
    if (cancel) m_controller->setNoteVelocity(m_sequenceDrag.clip.trackId.toStdString(),
        m_sequenceDrag.clip.clipId.toStdString(), m_sequenceDrag.noteId, m_sequenceDrag.velocity);
    m_controller->endNoteEdit("Change Note Velocity");
    QToolTip::hideText();
    emit projectEdited();
    update();
}

void TimelineWidget::showSequenceControl(const ClipHit& hit, SequencePart part, const QPoint& globalPos) {
    const auto* clip = findClipModel(hit.trackId, hit.clipId);
    if (!clip) return;
    if (part == SequencePart::Length) {
        const auto start = clip->startSeconds, offset = clip->offsetSeconds, step = clip->midiView.stepBeats;
        const int initial = int(std::lround(std::clamp(
            daw::secondsToBeats(clip->durationSeconds, m_controller->tempo()) / step, 1.0, 65536.0)));
        bool accepted = false;
        const int steps = QInputDialog::getInt(this, tr("Sequence length"), tr("Steps:"), initial, 1, 65536, 1, &accepted);
        if (!accepted || !m_controller->sharedEditingAllowed()) return;
        const auto track = hit.trackId.toStdString(), id = hit.clipId.toStdString();
        m_controller->beginClipTrimEdit(track, id);
        m_controller->setClipTrim(track, id, start, offset, daw::beatsToSeconds(steps * step, m_controller->tempo()));
        m_controller->endClipTrimEdit("Resize Sequence");
        emit projectEdited(); update(); return;
    }
    QMenu menu(this);
    populateSequenceMenu(menu, hit);
    const auto name = part == SequencePart::Pitch ? "SequencePitchMenu" : "SequenceGridMenu";
    if (auto* sub = menu.findChild<QMenu*>(QString::fromLatin1(name))) sub->exec(globalPos);
}

void TimelineWidget::populateSequenceMenu(QMenu& menu, const ClipHit& hit) {
    const auto* clip = findClipModel(hit.trackId, hit.clipId);
    if (!clip || clip->kind != daw::ClipKind::Midi) return;
    const auto view = clip->midiView;
    auto* modes = new QActionGroup(&menu);
    for (bool sequence : {false, true}) {
        auto* action = menu.addAction(sequence ? tr("Sequence view") : tr("MIDI view"));
        action->setObjectName(sequence ? "SequenceViewAction" : "MidiViewAction");
        action->setCheckable(true); action->setChecked(view.sequence == sequence);
        action->setEnabled(!sequence || sequenceAvailable(*clip)); modes->addAction(action);
        connect(action, &QAction::triggered, this, [this, hit, sequence] {
            if (const auto* current = findClipModel(hit.trackId, hit.clipId)) {
                auto next = current->midiView; next.sequence = sequence; applyMidiView(hit, next);
            }
        });
    }
    if (!view.sequence || !sequenceAvailable(*clip)) return;
    auto* pitch = menu.addMenu(tr("Step note · %1").arg(seq::pitchName(view.pitch)));
    pitch->setObjectName("SequencePitchMenu");
    auto* group = new QActionGroup(pitch);
    const auto addPitch = [&](QMenu* target, int note) {
        auto* action = target->addAction(seq::pitchName(note));
        action->setObjectName(QStringLiteral("SequencePitch%1").arg(note));
        action->setCheckable(true); action->setChecked(note == view.pitch); group->addAction(action);
        connect(action, &QAction::triggered, this, [this, hit, note] {
            if (const auto* current = findClipModel(hit.trackId, hit.clipId)) {
                auto next = current->midiView; next.pitch = note; applyMidiView(hit, next);
            }
        });
    };
    for (int note = (view.pitch / 12) * 12; note < std::min(128, (view.pitch / 12 + 1) * 12); ++note) addPitch(pitch, note);
    pitch->addSeparator();
    auto* all = pitch->addMenu(tr("All notes"));
    for (int octave = 0; octave <= 10; ++octave) {
        if (octave == view.pitch / 12) continue;
        auto* sub = all->addMenu(tr("Octave %1").arg(octave));
        for (int note = octave * 12; note < std::min(128, (octave + 1) * 12); ++note) addPitch(sub, note);
    }
    auto* grid = menu.addMenu(tr("Step grid · %1").arg(seq::divisionName(view.stepBeats)));
    grid->setObjectName("SequenceGridMenu");
    auto* grids = new QActionGroup(grid);
    for (const auto& d : seq::divisions) {
        auto* action = grid->addAction(QString::fromLatin1(d.name));
        action->setCheckable(true); action->setChecked(std::abs(view.stepBeats - d.beats) < 1e-8); grids->addAction(action);
        connect(action, &QAction::triggered, this, [this, hit, step = d.beats] {
            if (const auto* current = findClipModel(hit.trackId, hit.clipId)) {
                auto next = current->midiView; next.stepBeats = step; applyMidiView(hit, next);
            }
        });
    }
    auto* length = menu.addAction(tr("Sequence length…"));
    connect(length, &QAction::triggered, this, [this, hit] { showSequenceControl(hit, SequencePart::Length, {}); });
    SequenceHit note;
    if (hitTestSequence(mapFromGlobal(QCursor::pos()), note) && !note.noteId.empty() &&
        note.clip.clipId == hit.clipId) {
        auto* velocity = menu.addAction(tr("Note velocity…"));
        connect(velocity, &QAction::triggered, this, [this, note] {
            bool accepted = false;
            const int value = QInputDialog::getInt(this, tr("Note velocity"), tr("Velocity (1–127):"),
                note.velocity, 1, 127, 1, &accepted);
            if (!accepted || !m_controller->sharedEditingAllowed()) return;
            m_controller->beginNoteEdit(note.clip.trackId.toStdString(), note.clip.clipId.toStdString());
            m_controller->setNoteVelocity(note.clip.trackId.toStdString(), note.clip.clipId.toStdString(), note.noteId, value);
            m_controller->endNoteEdit("Change Note Velocity");
            emit projectEdited(); update();
        });
    }
}
