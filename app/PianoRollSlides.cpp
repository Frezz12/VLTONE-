#include "EngineController.hpp"
#include "PianoRollWindow.hpp"
#include "SlideCurveEditor.hpp"
#include "SlideNotes.hpp"
#include "Theme.hpp"
#include "UiConstants.hpp"
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
// Restore just this gesture, retaining list order and unrelated remote edits.
void restoreSlide(std::vector<daw::SlideNoteModel> &current,
                  const std::vector<daw::SlideNoteModel> &before, const std::string &id) {
    auto old =
        std::find_if(before.begin(), before.end(), [&](const auto &s) { return s.id == id; });
    auto now =
        std::find_if(current.begin(), current.end(), [&](const auto &s) { return s.id == id; });
    if (old == before.end()) {
        if (now != current.end())
            current.erase(now);
    } else if (now != current.end())
        *now = *old;
}
} // namespace

QRectF PianoRollView::slideRect(const daw::SlideNoteModel &s) const {
    double pitch = s.points.empty() ? 60 : s.points.back().value;
    return {beatsToX(s.startBeats), pitchToY(int(std::lround(pitch))) + 1,
            std::max(5., s.lengthBeats * pxPerBeat()), std::max(7., m_rowHeight - 2)};
}
std::string PianoRollView::slideAt(QPointF p) const {
    if (!clip() || p.x() < keyboardWidth() || p.y() < ui::kRulerHeight || p.y() >= laneTop())
        return {};
    const auto &list = daw::slides::editable(*clip());
    for (auto i = list.rbegin(); i != list.rend(); ++i)
        if (slideRect(*i).adjusted(0, -2, 0, 2).contains(p))
            return i->id;
    return {};
}
void PianoRollView::paintSlides(QPainter &p) {
    const auto *c = clip();
    if (!c)
        return;
    const auto &notes = daw::midiNotes(*c);
    const auto &list = daw::slides::editable(*c);
    p.save();
    p.setClipRect(QRectF(keyboardWidth(), ui::kRulerHeight, width() - keyboardWidth(),
                         laneTop() - ui::kRulerHeight),
                  Qt::IntersectClip);
    p.setRenderHint(QPainter::Antialiasing);
    const auto revision = m_controller->midiNotesRevision(m_trackId.toStdString());
    if (m_slidePaintRevision != revision || m_slidePaintClip != c->id) {
        m_slidePaintCurves = daw::slides::compile(notes, list);
        m_slidePaintRevision = revision;
        m_slidePaintClip = c->id;
    }
    const auto &curves = m_slidePaintCurves;
    for (const auto &n : notes) {
        auto curve = curves.find(n.id);
        if (curve == curves.end() || curve->second.empty())
            continue;
        QPainterPath path;
        bool first = true;
        double end = std::min(n.startBeats + n.lengthBeats, xToBeats(width()));
        double begin =
            std::max({n.startBeats, curve->second.front().beats, xToBeats(keyboardWidth())});
        for (double b = begin; b <= end + 1e-7; b += std::max(.002, 1.5 / pxPerBeat())) {
            double pitch =
                n.pitch + daw::engine::curve::valueAt(curve->second, std::min(b, end), 0);
            QPointF q(beatsToX(std::min(b, end)),
                      pitchToY(0) - pitch * m_rowHeight + m_rowHeight * .5);
            if (first) {
                path.moveTo(q);
                first = false;
            } else
                path.lineTo(q);
        }
        QColor ink = Theme::automationAccent();
        ink.setAlpha(170);
        p.setPen(QPen(ink, 1.3));
        p.drawPath(path);
    }
    auto draw = [&](const daw::SlideNoteModel &s, bool preview) {
        auto r = slideRect(s);
        const daw::NoteModel *anchor = nullptr;
        for (const auto &n : notes)
            if (n.id == s.referenceNoteId && !n.muted && n.startBeats <= s.startBeats &&
                n.startBeats + n.lengthBeats > s.startBeats)
                anchor = &n;
        QColor ink = anchor && !s.muted ? Theme::automationAccent() : th().textSecondary;
        QColor fill = ink;
        fill.setAlpha(preview ? 20 : 45);
        p.setBrush(fill);
        p.setPen(QPen(ink, s.id == m_selectedSlide ? 2 : 1,
                      preview || !anchor ? Qt::DashLine : Qt::SolidLine));
        p.drawRoundedRect(r, 3, 3);
        double activeEnd = s.startBeats;
        if (anchor)
            for (const auto &n : notes)
                if (!n.muted && std::find(s.targetNoteIds.begin(), s.targetNoteIds.end(), n.id) !=
                                    s.targetNoteIds.end())
                    activeEnd = std::max(activeEnd, n.startBeats + n.lengthBeats);
        if (activeEnd < s.startBeats + s.lengthBeats) {
            double left = std::max(r.left(), beatsToX(activeEnd));
            p.fillRect(QRectF(left, r.top(), r.right() - left, r.height()),
                       QBrush(ink, Qt::BDiagPattern));
        }
        if (anchor && (preview || s.id == m_selectedSlide)) {
            p.setPen(QPen(ink, 1, Qt::DashLine));
            p.drawLine(QPointF(r.left(), r.center().y()),
                       QPointF(r.left(), noteRect(*anchor).center().y()));
            for (const auto &n : notes)
                if (std::find(s.targetNoteIds.begin(), s.targetNoteIds.end(), n.id) !=
                    s.targetNoteIds.end()) {
                    p.setBrush(Qt::NoBrush);
                    p.drawRoundedRect(noteRect(n), 2, 2);
                }
        }
        if (!s.points.empty() && r.width() > 24) {
            auto points = s.points;
            auto actual = curves.find(s.referenceNoteId);
            if (!s.resumed && anchor && actual != curves.end())
                points.front().value =
                    anchor->pitch + daw::engine::curve::valueAt(actual->second, s.startBeats, 0);
            double lo = 127, hi = 0;
            for (auto &pt : points) {
                lo = std::min(lo, pt.value);
                hi = std::max(hi, pt.value);
            }
            double range = std::max(1., hi - lo);
            QPainterPath path;
            bool first = true;
            for (std::size_t i = 0; i + 1 < points.size(); ++i) {
                auto a = points[i], b = points[i + 1];
                for (int j = 0; j <= 12; ++j) {
                    double t = j / 12.;
                    double val =
                        a.value + (b.value - a.value) * daw::engine::curve::shapeT(
                                                            t, daw::toCurveShape(a.shape), a.curve);
                    QPointF q(r.left() + 12 +
                                  (r.width() - 15) * (a.beats + (b.beats - a.beats) * t),
                              r.bottom() - 2 - (val - lo) / range * std::max(2., r.height() - 4));
                    if (first) {
                        path.moveTo(q);
                        first = false;
                    } else
                        path.lineTo(q);
                }
            }
            p.setPen(QPen(ink, 1));
            p.drawPath(path);
        }
        p.setPen(ink);
        p.drawText(r.adjusted(2, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   anchor ? QString::fromUtf8("↗") : QStringLiteral("!"));
    };
    for (const auto &s : list)
        draw(s, false);
    if (m_tool == Tool::Slide && !m_slideGesture && m_pointerInside &&
        m_pointer.x() >= keyboardWidth() && m_pointer.y() >= ui::kRulerHeight &&
        m_pointer.y() < laneTop() && slideAt(m_pointer).empty()) {
        std::vector<std::string> selected;
        for (const auto &id : m_selected)
            selected.push_back(id.toStdString());
        auto preview = daw::slides::create(
            notes, std::max(0., snapBeats(xToBeats(m_pointer.x()), m_snapEnabled)),
            std::max(.25, effectiveGridBeats()), yToPitch(m_pointer.y()), selected, m_slideChord);
        draw(preview, true);
    }
    p.restore();
}
void PianoRollView::publishSlides(const std::vector<daw::SlideNoteModel> &value, bool undo) {
    m_controller->setClipSlideNotes(m_trackId.toStdString(), m_clipId.toStdString(), value,
                                    "Edit Slide Notes", undo);
    update();
    if (undo)
        emit edited();
}
bool PianoRollView::slidePress(QMouseEvent *e) {
    if (!clip() || e->position().x() < keyboardWidth() || e->position().y() < ui::kRulerHeight ||
        e->position().y() >= laneTop())
        return false;
    auto id = slideAt(e->position());
    if (id.empty() && m_tool != Tool::Slide) {
        m_selectedSlide.clear();
        return false;
    }
    if (e->button() != Qt::LeftButton && e->button() != Qt::RightButton)
        return false;
    if (!m_controller->canEditSlideNotes()) {
        emit statusChanged(tr("Slide editing requires collaboration protocol 5. "
                              "Reconnect to an updated session."));
        return true;
    }
    auto list = daw::slides::editable(*clip());
    if (e->button() == Qt::RightButton || m_tool == Tool::Erase) {
        if (id.empty())
            return m_tool == Tool::Slide;
        std::erase_if(list, [&](const auto &s) { return s.id == id; });
        publishSlides(list, true);
        return true;
    }
    if (m_tool == Tool::Mute && !id.empty()) {
        for (auto &s : list)
            if (s.id == id)
                s.muted = !s.muted;
        publishSlides(list, true);
        return true;
    }
    m_slideBefore = list;
    m_slidePress = e->position();
    m_slideCreating = id.empty();
    m_slideResize = false;
    if (m_slideCreating) {
        std::vector<std::string> selected;
        for (const auto &key : m_selected)
            selected.push_back(key.toStdString());
        auto s =
            daw::slides::create(daw::midiNotes(*clip()),
                                std::max(0., snapBeats(xToBeats(e->position().x()), m_snapEnabled)),
                                std::max(.25, effectiveGridBeats()), yToPitch(e->position().y()),
                                selected, m_slideChord);
        id = s.id;
        if (s.referenceNoteId.empty())
            emit statusChanged(tr("Inactive slide: no base note is sounding here. Rebind it or "
                                  "extend the base note."));
        list.push_back(s);
        publishSlides(list, false);
    } else {
        auto i = std::find_if(list.begin(), list.end(), [&](const auto &s) { return s.id == id; });
        m_slideResize = std::abs(e->position().x() - slideRect(*i).right()) < 7;
    }
    m_selectedSlide = id;
    m_slideGesture = true;
    e->accept();
    update();
    return true;
}
bool PianoRollView::slideMove(QMouseEvent *e) {
    if (!m_slideGesture) {
        if (m_tool == Tool::Slide)
            update();
        return false;
    }
    if (!(e->buttons() & Qt::LeftButton)) {
        slideRelease();
        return true;
    }
    if (!clip()) {
        m_slideGesture = false;
        return true;
    }
    auto list = daw::slides::editable(*clip());
    auto it = std::find_if(list.begin(), list.end(),
                           [&](const auto &s) { return s.id == m_selectedSlide; });
    if (it == list.end())
        return true;
    const bool snap = m_snapEnabled != bool(e->modifiers() & Qt::AltModifier);
    if (m_slideCreating || m_slideResize)
        it->lengthBeats =
            std::max(1. / 960., snapBeats(xToBeats(e->position().x()), snap) - it->startBeats);
    else {
        auto old = std::find_if(m_slideBefore.begin(), m_slideBefore.end(),
                                [&](const auto &s) { return s.id == m_selectedSlide; });
        if (old != m_slideBefore.end()) {
            it->startBeats =
                std::max(0., snapBeats(old->startBeats +
                                           (e->position().x() - m_slidePress.x()) / pxPerBeat(),
                                       snap));
            double delta = yToPitch(e->position().y()) - yToPitch(m_slidePress.y());
            it->points = old->points;
            for (auto &pt : it->points)
                pt.value = std::clamp(pt.value + delta, 0., 127.);
        }
    }
    publishSlides(list, false);
    return true;
}
bool PianoRollView::slideRelease() {
    if (!m_slideGesture)
        return false;
    m_slideGesture = false;
    if (!clip())
        return true;
    auto result = daw::slides::editable(*clip());
    auto before = result;
    restoreSlide(before, m_slideBefore, m_selectedSlide);
    publishSlides(before, false);
    publishSlides(result, true);
    m_slideBefore.clear();
    return true;
}
void PianoRollView::cancelSlideGesture() {
    if (!m_slideGesture)
        return;
    m_slideGesture = false;
    if (!clip())
        return;
    auto before = daw::slides::editable(*clip());
    restoreSlide(before, m_slideBefore, m_selectedSlide);
    publishSlides(before, false);
    m_slideBefore.clear();
}
bool PianoRollView::slideKey(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && m_slideGesture) {
        cancelSlideGesture();
        return true;
    }
    if (e->key() == Qt::Key_6 && e->modifiers() == Qt::NoModifier) {
        setTool(Tool::Slide);
        return true;
    }
    if (m_selectedSlide.empty() || !clip())
        return false;
    if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
        auto list = daw::slides::editable(*clip());
        std::erase_if(list, [&](const auto &s) { return s.id == m_selectedSlide; });
        m_selectedSlide.clear();
        publishSlides(list, true);
        return true;
    }
    if (e->key() == Qt::Key_Return) {
        openSlideEditor(m_selectedSlide);
        return true;
    }
    return false;
}
void PianoRollView::openSlideEditor(const std::string &id) {
    if (!clip())
        return;
    if (!m_controller->canEditSlideNotes()) {
        emit statusChanged(tr("Slide editing requires collaboration protocol 5. "
                              "Reconnect to an updated session."));
        return;
    }
    if (m_slideEditor)
        m_slideEditor->close();
    auto list = daw::slides::editable(*clip());
    auto it = std::find_if(list.begin(), list.end(), [&](const auto &s) { return s.id == id; });
    if (it == list.end())
        return;
    auto slide = *it;
    m_selectedSlide = id;
    double initial = slide.points.front().value;
    auto previous = list;
    std::erase_if(previous, [&](const auto &s) { return s.startBeats >= slide.startBeats; });
    const auto curves = daw::slides::compile(daw::midiNotes(*clip()), previous);
    for (const auto &n : daw::midiNotes(*clip()))
        if (n.id == slide.referenceNoteId) {
            auto c = curves.find(n.id);
            initial = n.pitch + (c == curves.end()
                                     ? 0
                                     : daw::engine::curve::valueAt(c->second, slide.startBeats, 0));
        }
    if (slide.resumed)
        initial = slide.points.front().value;
    auto *editor = new SlideCurveEditor(slide, initial, this);
    m_slideEditor = editor;
    const bool linked = std::any_of(
        daw::midiNotes(*clip()).begin(), daw::midiNotes(*clip()).end(), [&](const auto &n) {
            return n.id == slide.referenceNoteId && !n.muted && n.startBeats <= slide.startBeats &&
                   n.startBeats + n.lengthBeats > slide.startBeats;
        });
    if (!linked)
        editor->setWindowTitle(tr("Slide curve — inactive: rebind or extend the base note"));
    if (const auto *track = m_controller->project().findTrack(m_trackId.toStdString())) {
        auto status = m_controller->instrumentSlideStatus(track->id);
        double basePitch = initial;
        for (const auto &note : daw::midiNotes(*clip()))
            if (note.id == slide.referenceNoteId)
                basePitch = note.pitch;
        if (status.mode == daw::plugins::SlideDelivery::PitchBend ||
            status.mode == daw::plugins::SlideDelivery::MPE)
            editor->setBendRange(status.mode == daw::plugins::SlideDelivery::MPE &&
                                         track->instrument.slideDelivery == 0 &&
                                         track->instrument.slideBendRange == 2
                                     ? 48
                                     : track->instrument.slideBendRange,
                                 basePitch);
        else if (status.mode == daw::plugins::SlideDelivery::NoteExpression &&
                 track->instrument.format != daw::PluginFormat::Internal)
            editor->setBendRange(120, basePitch);
    }
    auto committed = std::make_shared<daw::SlideNoteModel>(slide);
    editor->changed = [this, committed, id](const daw::SlideNoteModel &s, bool commit) {
        if (!clip())
            return;
        auto result = daw::slides::editable(*clip());
        auto it = std::find_if(result.begin(), result.end(),
                               [&](const auto &item) { return item.id == id; });
        if (it == result.end())
            return;
        *it = s;
        if (commit) {
            auto before = result;
            for (auto &item : before)
                if (item.id == id)
                    item = *committed;
            publishSlides(before, false);
            publishSlides(result, true);
            *committed = s;
        } else
            publishSlides(result, false);
    };
    auto modelRevision = std::make_shared<std::uint64_t>(~std::uint64_t(0));
    auto *modelTimer = new QTimer(editor);
    modelTimer->setInterval(100);
    connect(modelTimer, &QTimer::timeout, editor, [this, editor, id, committed, modelRevision] {
        if (editor->editing())
            return;
        if (!clip()) {
            editor->close();
            return;
        }
        const auto revision = m_controller->midiNotesRevision(m_trackId.toStdString());
        if (*modelRevision == revision)
            return;
        *modelRevision = revision;
        const auto &list = daw::slides::editable(*clip());
        auto current =
            std::find_if(list.begin(), list.end(), [&](const auto &s) { return s.id == id; });
        if (current == list.end()) {
            editor->close();
            return;
        }
        double start = current->points.front().value;
        if (!current->resumed) {
            const auto curves = daw::slides::compile(daw::midiNotes(*clip()), list);
            for (const auto &n : daw::midiNotes(*clip()))
                if (n.id == current->referenceNoteId) {
                    auto found = curves.find(n.id);
                    start = n.pitch + (found == curves.end()
                                           ? 0
                                           : daw::engine::curve::valueAt(found->second,
                                                                         current->startBeats, 0));
                }
        }
        *committed = *current;
        editor->refresh(*current, start);
    });
    modelTimer->start();
    editor->extendBase = [this, id] {
        if (!clip())
            return;
        const auto &slides = daw::slides::editable(*clip());
        auto s =
            std::find_if(slides.begin(), slides.end(), [&](const auto &a) { return a.id == id; });
        if (s == slides.end())
            return;
        auto notes = daw::midiNotes(*clip());
        for (auto &n : notes)
            if (std::find(s->targetNoteIds.begin(), s->targetNoteIds.end(), n.id) !=
                s->targetNoteIds.end())
                n.lengthBeats =
                    std::max(n.lengthBeats, s->startBeats + s->lengthBeats - n.startBeats);
        m_controller->setClipNotes(m_trackId.toStdString(), m_clipId.toStdString(), notes,
                                   "Extend Slide Base Notes");
        update();
        emit edited();
    };
    editor->rebind = [this, id] {
        if (!clip())
            return;
        auto list = daw::slides::editable(*clip());
        for (auto &s : list)
            if (s.id == id) {
                std::vector<std::string> selected;
                for (const auto &key : m_selected)
                    selected.push_back(key.toStdString());
                auto binding =
                    daw::slides::create(daw::midiNotes(*clip()), s.startBeats, s.lengthBeats,
                                        s.points.back().value, selected, s.chord);
                s.referenceNoteId = binding.referenceNoteId;
                s.targetNoteIds = binding.targetNoteIds;
            }
        publishSlides(list, true);
    };
    struct PreviewState {
        bool active = false, wasPlaying = false;
        std::string auditionTrack;
        double before = 0, end = 0;
    };
    auto preview = std::make_shared<PreviewState>();
    auto *auditionTimer = new QTimer(editor);
    auditionTimer->setInterval(15);
    auto restore = [this, preview] {
        if (!preview->active)
            return;
        preview->active = false;
        m_controller->setExclusiveAuditionTrack(preview->auditionTrack);
        m_controller->stop();
        m_controller->seekSeconds(preview->before);
        if (preview->wasPlaying)
            m_controller->play();
    };
    connect(auditionTimer, &QTimer::timeout, editor, [this, preview, restore] {
        if (preview->active &&
            (!m_controller->isPlaying() || m_controller->positionSeconds() >= preview->end))
            restore();
    });
    connect(editor, &QObject::destroyed, this, [restore] { restore(); });
    editor->audition = [this, id, preview, auditionTimer, restore] {
        if (!clip())
            return;
        if (preview->active) {
            restore();
            return;
        }
        double start = 1e100, end = 0;
        for (const auto &s : daw::slides::editable(*clip()))
            if (s.id == id)
                for (const auto &n : daw::midiNotes(*clip()))
                    if (std::find(s.targetNoteIds.begin(), s.targetNoteIds.end(), n.id) !=
                        s.targetNoteIds.end()) {
                        start = std::min(start, n.startBeats);
                        end = std::max(end, n.startBeats + n.lengthBeats);
                    }
        if (end <= start)
            return;
        preview->before = m_controller->positionSeconds();
        preview->wasPlaying = m_controller->isPlaying();
        preview->auditionTrack = m_controller->exclusiveAuditionTrackId();
        m_controller->setExclusiveAuditionTrack(m_trackId.toStdString());
        preview->active = true;
        preview->end = clip()->startSeconds + end * 60. / m_controller->tempo();
        m_controller->seekSeconds(clip()->startSeconds + start * 60. / m_controller->tempo());
        m_controller->play();
        auditionTimer->start();
    };
    editor->move(
        mapToGlobal(QPoint(int(slideRect(slide).left()), int(slideRect(slide).bottom() + 12))));
    editor->show();
    auto available = editor->screen()->availableGeometry();
    editor->move(std::clamp(editor->x(), available.left(),
                            std::max(available.left(), available.right() - editor->width())),
                 std::clamp(editor->y(), available.top(),
                            std::max(available.top(), available.bottom() - editor->height())));
}
