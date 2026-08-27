#include "AutomationEditorWindow.hpp"

#include "Controls.hpp"
#include "EngineController.hpp"
#include "Icons.hpp"
#include "Theme.hpp"

#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <cmath>

namespace at = daw::autotools;

namespace {

/// How near the pointer has to be to grab a breakpoint.
constexpr double kGrab = 7.0;
constexpr double kPointRadius = 4.0;

/// The gutters around the plot: the value axis on the left, the bar ruler
/// underneath.
constexpr double kAxisWidth = 62.0;
constexpr double kRulerHeight = 20.0;
constexpr double kPad = 10.0;

/// The rate menu, as beats per cycle. Bars are 4 beats here on purpose — the
/// list is a set of musical divisions, not a reading of the meter, and a
/// triplet feel wants the same twelve entries whatever the bar length is.
struct RateChoice { const char* label; double beats; };
const RateChoice kRates[] = {
    {"1/16", 0.25}, {"1/8", 0.5},  {"1/4", 1.0},   {"1/2", 2.0},
    {QT_TRANSLATE_NOOP("AutomationEditorWindow", "1 bar"), 4.0},
    {QT_TRANSLATE_NOOP("AutomationEditorWindow", "2 bars"), 8.0},
    {QT_TRANSLATE_NOOP("AutomationEditorWindow", "4 bars"), 16.0},
    {QT_TRANSLATE_NOOP("AutomationEditorWindow", "8 bars"), 32.0},
};

const at::Points& emptyPoints() {
    static const at::Points empty;
    return empty;
}

}   // namespace

// ── AutomationCurveView ─────────────────────────────────────────────────────

AutomationCurveView::AutomationCurveView(daw::EngineController* controller,
                                         QWidget* parent)
    : QWidget(parent), m_controller(controller) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(220);
    setCursor(Qt::CrossCursor);
}

void AutomationCurveView::setClip(const QString& trackId, const QString& clipId) {
    m_trackId = trackId;
    m_clipId = clipId;
    m_previewing = false;
    m_gesture = false;
    m_dragPoint = -1;
    m_dragPoints.clear();
    clearSelection();
    update();
}

void AutomationCurveView::setTool(Tool tool) {
    m_tool = tool;
    setCursor(tool == Tool::Draw ? Qt::PointingHandCursor : Qt::CrossCursor);
}

void AutomationCurveView::setSnapBeats(double beats) {
    m_snapBeats = std::max(0.0, beats);
    update();
}

void AutomationCurveView::setNewPointShape(daw::AutomationSegment shape) {
    m_newShape = shape;
}

QPoint AutomationCurveView::pointPositionForTest(int index) const {
    const at::Points& points = curve();
    if (index < 0 || index >= int(points.size())) return {};
    const auto& point = points[std::size_t(index)];
    return QPoint(qRound(beatsToX(point.beats)), qRound(valueToY(point.value)));
}

const daw::ClipModel* AutomationCurveView::clip() const {
    if (!m_controller) return nullptr;
    const daw::TrackModel* track =
        m_controller->project().findTrack(m_trackId.toStdString());
    if (!track) return nullptr;
    for (const daw::ClipModel& c : track->clips) {
        if (c.id == m_clipId.toStdString()) return &c;
    }
    return nullptr;
}

const at::Points& AutomationCurveView::curve() const {
    if (m_previewing) return m_preview;
    const daw::ClipModel* c = clip();
    return c ? c->automation.points : emptyPoints();
}

double AutomationCurveView::lengthBeats() const {
    const daw::ClipModel* c = clip();
    if (!c || !m_controller) return 16.0;
    return std::max(0.25, daw::secondsToBeats(c->durationSeconds,
                                              m_controller->project().tempo));
}

QRectF AutomationCurveView::plot() const {
    return QRectF(kAxisWidth, kPad, std::max(20.0, width() - kAxisWidth - kPad),
                  std::max(20.0, height() - kPad - kRulerHeight));
}

double AutomationCurveView::beatsToX(double beats) const {
    const QRectF box = plot();
    return box.left() + box.width() * (beats / lengthBeats());
}

double AutomationCurveView::xToBeats(double x) const {
    const QRectF box = plot();
    if (box.width() <= 0.0) return 0.0;
    return std::clamp((x - box.left()) / box.width() * lengthBeats(), 0.0,
                      lengthBeats());
}

double AutomationCurveView::valueToY(double value) const {
    const QRectF box = plot();
    return box.bottom() - box.height() * std::clamp(value, 0.0, 1.0);
}

double AutomationCurveView::yToValue(double y) const {
    const QRectF box = plot();
    if (box.height() <= 0.0) return 0.0;
    return std::clamp((box.bottom() - y) / box.height(), 0.0, 1.0);
}

double AutomationCurveView::snap(double beats) const {
    if (m_snapBeats <= 0.0) return beats;
    return std::round(beats / m_snapBeats) * m_snapBeats;
}

int AutomationCurveView::pointAt(const QPointF& pos) const {
    const at::Points& points = curve();
    int best = -1;
    double bestDistance = kGrab;
    const double atBeat = xToBeats(pos.x());
    const double beatMargin =
        lengthBeats() * kGrab / std::max(1.0, plot().width());
    const auto first = std::lower_bound(
        points.begin(), points.end(), atBeat - beatMargin,
        [](const daw::AutomationPoint& point, double beat) {
            return point.beats < beat;
        });
    for (auto point = first; point != points.end(); ++point) {
        if (point->beats > atBeat + beatMargin) break;
        const std::size_t i = std::size_t(point - points.begin());
        const QPointF at(beatsToX(points[i].beats), valueToY(points[i].value));
        const double d = std::hypot(at.x() - pos.x(), at.y() - pos.y());
        if (d <= bestDistance) {
            bestDistance = d;
            best = int(i);
        }
    }
    return best;
}

int AutomationCurveView::segmentAt(const QPointF& pos) const {
    const at::Points& points = curve();
    if (points.size() < 2) return -1;
    const double beats = xToBeats(pos.x());
    const auto atOrAfter = std::lower_bound(
        points.begin(), points.end(), beats,
        [](const daw::AutomationPoint& point, double beat) {
            return point.beats < beat;
        });
    if (atOrAfter == points.end()) return -1;
    if (atOrAfter == points.begin())
        return beats >= points.front().beats ? 0 : -1;
    // At an exact internal breakpoint the old linear scan chose the segment on
    // its left (`<=` on both ends); retain that gesture boundary.
    return int((atOrAfter - points.begin()) - 1);
}

at::Range AutomationCurveView::range() const {
    if (!m_hasSelection) return at::Range{0.0, lengthBeats()};
    return at::Range{std::min(m_selectFrom, m_selectTo),
                     std::max(m_selectFrom, m_selectTo)};
}

void AutomationCurveView::selectAll() {
    m_hasSelection = true;
    m_selectFrom = 0.0;
    m_selectTo = lengthBeats();
    emit selectionChanged();
    update();
}

void AutomationCurveView::clearSelection() {
    if (!m_hasSelection) return;
    m_hasSelection = false;
    emit selectionChanged();
    update();
}

void AutomationCurveView::beginGesture() {
    if (m_gesture) return;
    const daw::ClipModel* c = clip();
    m_before = c ? c->automation.points : at::Points{};
    m_gesture = true;
}

void AutomationCurveView::pushLive(at::Points points) {
    if (!m_controller) return;
    if (m_previewing) m_preview = points;
    m_controller->setAutomationPoints(m_trackId.toStdString(),
                                      m_clipId.toStdString(), std::move(points));
    emit liveEdited();
    update();
}

void AutomationCurveView::commit(const QString& label) {
    if (!m_controller || !m_gesture) return;
    m_gesture = false;
    m_controller->commitAutomationEdit(m_trackId.toStdString(),
                                       m_clipId.toStdString(), m_before,
                                       label.toStdString());
    emit edited();
    update();
}

void AutomationCurveView::showPreview(const at::Points& points) {
    beginGesture();
    m_previewing = true;
    pushLive(points);
}

void AutomationCurveView::cancelPreview() {
    if (!m_previewing) return;
    m_previewing = false;
    const at::Points before = m_before;
    m_preview.clear();
    if (m_gesture) {
        // Straight back through the live path: the gesture never became an undo
        // entry, so there is nothing to undo — there is only a value the engine
        // is still holding that has to stop being held.
        m_gesture = false;
        pushLive(before);
    }
    update();
}

void AutomationCurveView::commitPreview(const QString& label) {
    if (!m_previewing) return;
    m_previewing = false;
    m_preview.clear();
    commit(label);
}

at::Points AutomationCurveView::points() const { return curve(); }

void AutomationCurveView::applyPoints(const at::Points& points, const QString& label) {
    beginGesture();
    pushLive(points);
    commit(label);
}

QString AutomationCurveView::readoutFor(double beats, double value) const {
    const daw::ClipModel* c = clip();
    if (!c || !m_controller) return {};
    const int numerator = std::max(1, m_controller->project().timeSigNumerator);
    const int bar = int(beats) / numerator + 1;
    const double beatInBar = std::fmod(beats, double(numerator)) + 1.0;
    return tr("%1.%2   %3")
        .arg(bar)
        .arg(beatInBar, 0, 'f', 2)
        .arg(QString::fromStdString(
            m_controller->automationValueText(c->automation.target, value)));
}

void AutomationCurveView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const Theme& t = th();
    const QRectF box = plot();
    p.fillRect(rect(), t.background);

    const daw::ClipModel* c = clip();
    if (!c || !m_controller) return;
    const QColor accent = colorFromRgb(c->color);
    const double length = lengthBeats();
    const int numerator = std::max(1, m_controller->project().timeSigNumerator);

    // The well the curve lives in.
    p.setPen(Qt::NoPen);
    p.setBrush(mixColors(t.well(), accent, t.dark ? 0.12 : 0.08));
    p.drawRoundedRect(box, 5, 5);

    // The selection, behind everything: a stretch of time the transforms will
    // act on, so it has to read as a region rather than as a highlight on the
    // curve.
    if (m_hasSelection) {
        const double from = beatsToX(std::min(m_selectFrom, m_selectTo));
        const double to = beatsToX(std::max(m_selectFrom, m_selectTo));
        QColor wash = t.accent;
        wash.setAlpha(46);
        p.fillRect(QRectF(from, box.top(), std::max(1.0, to - from), box.height()),
                   wash);
        p.setPen(QPen(mixColors(t.accent, t.textPrimary, 0.3), 1.0));
        p.drawLine(QPointF(from, box.top()), QPointF(from, box.bottom()));
        p.drawLine(QPointF(to, box.top()), QPointF(to, box.bottom()));
    }

    // Beat and bar lines, and the same three value guides the lane draws — a
    // curve read here and read in the arrangement has to line up against the
    // same references.
    const double beatWidth = box.width() / length;
    p.setPen(QPen(mixColors(t.gridLine, t.background, 0.4), 1.0));
    for (double beat = 0.0; beat <= length + 1e-6; beat += 1.0) {
        const bool bar = std::fmod(beat, double(numerator)) < 1e-6;
        if (!bar && beatWidth < 9.0) continue;
        p.setPen(QPen(bar ? t.gridLineStrong : mixColors(t.gridLine, t.background, 0.4),
                      1.0));
        const double x = beatsToX(beat);
        p.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
    }
    if (m_snapBeats > 0.0 && box.width() / (length / m_snapBeats) > 6.0) {
        QColor faint = t.gridLine;
        faint.setAlpha(50);
        p.setPen(QPen(faint, 1.0, Qt::DotLine));
        for (double beat = 0.0; beat <= length + 1e-6; beat += m_snapBeats) {
            const double x = beatsToX(beat);
            p.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
        }
    }
    p.setPen(QPen(mixColors(t.gridLine, t.background, 0.25), 1.0));
    for (const double level : {0.25, 0.5, 0.75}) {
        const double y = valueToY(level);
        p.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
    }

    // The value axis, in the parameter's own units.
    p.setPen(t.textSecondary);
    QFont small = p.font();
    small.setPointSizeF(std::max(8.0, small.pointSizeF() - 2.0));
    p.setFont(small);
    for (const double level : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        const double y = valueToY(level);
        p.drawText(QRectF(0, y - 8, kAxisWidth - 8, 16),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString::fromStdString(m_controller->automationValueText(
                       c->automation.target, level)));
    }
    // Bar numbers under the plot.
    for (double beat = 0.0; beat <= length + 1e-6; beat += double(numerator)) {
        p.drawText(QRectF(beatsToX(beat) + 3, box.bottom() + 2, 48, kRulerHeight - 3),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   QString::number(int(beat) / numerator + 1));
    }

    // The curve, sampled per pixel so a bend or an S reads as its shape rather
    // than as the straight line between its ends.
    const at::Points& points = curve();
    const double fallback = c->automation.defaultValue;
    QPolygonF line;
    for (int x = int(box.left()); x <= int(box.right()); ++x) {
        const double beats = xToBeats(x);
        line << QPointF(x, valueToY(daw::automationValueAt(points, beats, fallback)));
    }
    if (line.size() >= 2) {
        QPolygonF under = line;
        under << QPointF(line.back().x(), box.bottom());
        under << QPointF(line.front().x(), box.bottom());
        QColor wash = accent;
        wash.setAlpha(t.dark ? 54 : 64);
        p.setPen(Qt::NoPen);
        p.setBrush(wash);
        p.drawPolygon(under);

        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(mixColors(t.background, accent, 0.25), 3.5));
        p.drawPolyline(line);
        p.setPen(QPen(mixColors(accent, t.textPrimary, 0.3), 2.0));
        p.drawPolyline(line);
    }

    // Breakpoints. The hovered one grows; one inside the selection wears the
    // accent, so "what will this transform touch" is answered by looking.
    p.setRenderHint(QPainter::Antialiasing, true);
    const at::Range selected = range();
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (points[i].beats > length + 1e-9) continue;
        const QPointF centre(beatsToX(points[i].beats), valueToY(points[i].value));
        const bool inRange = m_hasSelection && selected.contains(points[i].beats);
        const double radius = int(i) == m_hoverPoint ? kPointRadius + 2.0 : kPointRadius;
        p.setBrush(inRange ? t.accent : t.surfaceElevated);
        p.setPen(QPen(mixColors(accent, t.textPrimary, 0.35), 1.8));
        p.drawEllipse(centre, radius, radius);
        if (points[i].shape != daw::AutomationSegment::Linear) {
            // A shaped segment is marked at the point that owns it: a hollow
            // ring for a step, a filled core for an S.
            p.setPen(Qt::NoPen);
            p.setBrush(mixColors(accent, t.textPrimary, 0.35));
            if (points[i].shape == daw::AutomationSegment::SCurve)
                p.drawEllipse(centre, 1.8, 1.8);
        }
    }
    p.setRenderHint(QPainter::Antialiasing, false);

    // The pointer's own readout, drawn where the pointer is rather than in a
    // corner: shaping a curve is a thing done while looking at the curve.
    if (m_hasCursor && box.contains(m_cursor)) {
        const QString text = readoutFor(xToBeats(m_cursor.x()), yToValue(m_cursor.y()));
        const double textWidth = p.fontMetrics().horizontalAdvance(text) + 14.0;
        // Flipped to the other side of the pointer near the right edge, so the
        // one number you are reading is never the one off the screen.
        const double left = m_cursor.x() + textWidth + 16 > box.right()
                                ? m_cursor.x() - textWidth - 12
                                : m_cursor.x() + 12;
        const QRectF label(left, m_cursor.y() - 26, textWidth, 20);
        // The same chip the value bubbles wear everywhere else in the program:
        // an elevated surface with an accent hairline, not an inverted block.
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(t.accent, 1.0));
        p.setBrush(mixColors(t.surfaceElevated, t.background, 0.15));
        p.drawRoundedRect(label, 5, 5);
        p.setPen(t.textPrimary);
        p.drawText(label, Qt::AlignCenter, text);
        p.setRenderHint(QPainter::Antialiasing, false);
    }
}

void AutomationCurveView::mousePressEvent(QMouseEvent* ev) {
    const daw::ClipModel* c = clip();
    if (!c || !m_controller) return;
    const QPointF pos = ev->position();
    setFocus(Qt::MouseFocusReason);

    if (ev->button() != Qt::LeftButton) return;

    at::Points points = curve();
    const int hit = pointAt(pos);

    // Shift on empty space drags out the range the generators act on. A point
    // gets the more local meaning: lock its value and move it only in time.
    if ((ev->modifiers() & Qt::ShiftModifier) && hit < 0) {
        m_banding = true;
        m_bandAnchor = xToBeats(pos.x());
        m_hasSelection = true;
        m_selectFrom = m_selectTo = m_bandAnchor;
        emit selectionChanged();
        update();
        return;
    }

    if (m_tool == Tool::Draw) {
        beginGesture();
        m_drawing = true;
        const double beats = snap(xToBeats(pos.x()));
        points.push_back({beats, yToValue(pos.y()), m_newShape, 0.0});
        pushLive(points);
        return;
    }

    if (hit >= 0) {
        beginGesture();
        m_dragPoint = hit;
        m_dragPoints = points;
        setCursor((ev->modifiers() & Qt::ShiftModifier)
                      ? Qt::SizeHorCursor
                      : Qt::SizeAllCursor);
        return;
    }

    // Alt over a segment bends it, which is the one gesture that changes a
    // curve without adding anything to it.
    if (ev->modifiers() & Qt::AltModifier) {
        const int segment = segmentAt(pos);
        if (segment >= 0) {
            beginGesture();
            m_bendSegment = segment;
            m_bendStartY = pos.y();
            m_bendStartCurve = points[std::size_t(segment)].curve;
            return;
        }
    }

    if (!plot().contains(pos)) return;

    // Empty space: place a point and keep hold of it, so putting one down and
    // putting it where you meant is one gesture rather than two.
    beginGesture();
    const double beats = snap(xToBeats(pos.x()));
    points.push_back({beats, yToValue(pos.y()), m_newShape, 0.0});
    daw::normalizeAutomation(points);
    m_dragPoints = points;
    pushLive(points);
    for (std::size_t i = 0; i < m_dragPoints.size(); ++i) {
        if (std::abs(m_dragPoints[i].beats - beats) < 1e-9)
            m_dragPoint = int(i);
    }
}

void AutomationCurveView::mouseMoveEvent(QMouseEvent* ev) {
    const QPointF pos = ev->position();
    m_cursor = pos;
    m_hasCursor = true;

    if (m_banding) {
        m_selectTo = xToBeats(pos.x());
        emit selectionChanged();
        update();
        return;
    }

    if (m_drawing) {
        at::Points points = curve();
        const double beats = snap(xToBeats(pos.x()));
        const double value = yToValue(pos.y());
        // One point per grid slot (or per few pixels with the grid off): a
        // freehand line that lands a point on every mouse event is unusable
        // afterwards, and sounds identical.
        const double spacing = m_snapBeats > 0.0 ? m_snapBeats * 0.5
                                                 : xToBeats(6.0) - xToBeats(0.0);
        bool replaced = false;
        for (auto& point : points) {
            if (std::abs(point.beats - beats) < spacing) {
                point.value = value;
                point.shape = m_newShape;
                replaced = true;
            }
        }
        if (!replaced) points.push_back({beats, value, m_newShape, 0.0});
        daw::normalizeAutomation(points);
        pushLive(points);
        return;
    }

    if (m_dragPoint >= 0 && m_dragPoint < int(m_dragPoints.size())) {
        // Alt while dragging a point drops the snap, the way every other
        // fine-adjustment in the program does.
        const double moved = std::clamp((ev->modifiers() & Qt::AltModifier)
                                            ? xToBeats(pos.x())
                                            : snap(xToBeats(pos.x())),
                                        0.0, lengthBeats());
        const double value = (ev->modifiers() & Qt::ShiftModifier)
                                 ? m_dragPoints[std::size_t(m_dragPoint)].value
                                 : yToValue(pos.y());
        const double guard = m_snapBeats > 0.0
                                 ? m_snapBeats
                                 : lengthBeats() * 8.0 /
                                       std::max(1.0, plot().width());
        at::Points points = at::dragPoint(
            m_dragPoints, std::size_t(m_dragPoint), moved, value, guard);
        pushLive(points);
        setCursor((ev->modifiers() & Qt::ShiftModifier)
                      ? Qt::SizeHorCursor
                      : Qt::SizeAllCursor);
        emit readoutChanged(readoutFor(moved, value));
        return;
    }

    if (m_bendSegment >= 0) {
        at::Points points = curve();
        if (m_bendSegment >= int(points.size())) return;
        auto& point = points[std::size_t(m_bendSegment)];
        const double travel = std::max(40.0, plot().height() * 0.5);
        point.curve =
            std::clamp(m_bendStartCurve + (m_bendStartY - pos.y()) / travel, -1.0, 1.0);
        pushLive(points);
        emit readoutChanged(tr("Curve %1").arg(point.curve, 0, 'f', 2));
        return;
    }

    const int hover = pointAt(pos);
    if (hover != m_hoverPoint) {
        m_hoverPoint = hover;
        update();
    }
    if (plot().contains(pos)) {
        emit readoutChanged(readoutFor(xToBeats(pos.x()), yToValue(pos.y())));
    }
    update();
}

void AutomationCurveView::mouseReleaseEvent(QMouseEvent*) {
    if (m_banding) {
        m_banding = false;
        // A band with no width is a click on the background, which means "never
        // mind" rather than "select an instant".
        if (std::abs(m_selectTo - m_selectFrom) < 1e-6) clearSelection();
        emit selectionChanged();
        update();
        return;
    }
    if (m_drawing) {
        m_drawing = false;
        commit(tr("Draw Automation"));
        return;
    }
    if (m_dragPoint >= 0) {
        m_dragPoint = -1;
        m_dragPoints.clear();
        setCursor(m_tool == Tool::Draw ? Qt::PointingHandCursor
                                       : Qt::CrossCursor);
        commit(tr("Move Automation Point"));
        return;
    }
    if (m_bendSegment >= 0) {
        m_bendSegment = -1;
        commit(tr("Curve Automation"));
        return;
    }
}

void AutomationCurveView::mouseDoubleClickEvent(QMouseEvent* ev) {
    // Double-click mirrors the control itself: return the breakpoint to the
    // target's factory/neutral value. Delete remains explicit on Backspace and
    // in the context menu, so an imprecise reset never destroys curve timing.
    const int hit = pointAt(ev->position());
    if (hit < 0) return;
    const daw::ClipModel* c = clip();
    if (!c || !m_controller) return;
    at::Points points = curve();
    points[std::size_t(hit)].value =
        m_controller->automationResetValue(c->automation.target);
    beginGesture();
    pushLive(points);
    commit(tr("Reset Automation Point"));
    ev->accept();
}

void AutomationCurveView::keyPressEvent(QKeyEvent* ev) {
    if (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) {
        at::Points points = curve();
        const at::Range r = range();
        at::Points kept;
        for (const auto& point : points) {
            if (m_hasSelection ? !r.contains(point.beats)
                               : !(m_hoverPoint >= 0 &&
                                   std::abs(point.beats -
                                            points[std::size_t(m_hoverPoint)].beats) < 1e-9))
                kept.push_back(point);
        }
        if (kept.size() != points.size()) applyPoints(kept, tr("Delete Automation Points"));
        return;
    }
    if (ev->key() == Qt::Key_A && (ev->modifiers() & Qt::ControlModifier)) {
        selectAll();
        return;
    }
    if (ev->key() == Qt::Key_Escape) {
        clearSelection();
        return;
    }
    QWidget::keyPressEvent(ev);
}

void AutomationCurveView::leaveEvent(QEvent*) {
    m_hasCursor = false;
    m_hoverPoint = -1;
    update();
}

void AutomationCurveView::contextMenuEvent(QContextMenuEvent* ev) {
    const daw::ClipModel* c = clip();
    if (!c) return;
    const QPointF pos = ev->pos();
    const int hit = pointAt(pos);
    const int segment = segmentAt(pos);

    QMenu menu(this);
    QAction* remove = nullptr;
    if (hit >= 0) {
        remove = menu.addAction(tr("Delete Point"));
        menu.addSeparator();
    }
    QAction* linear = nullptr;
    QAction* hold = nullptr;
    QAction* scurve = nullptr;
    QAction* straighten = nullptr;
    if (segment >= 0) {
        auto* shapes = menu.addMenu(tr("Segment"));
        linear = shapes->addAction(tr("Linear"));
        hold = shapes->addAction(tr("Hold (step)"));
        scurve = shapes->addAction(tr("S-Curve"));
        shapes->addSeparator();
        straighten = shapes->addAction(tr("Straighten"));
    }
    menu.addSeparator();
    QAction* selectAllAction = menu.addAction(tr("Select All"));
    QAction* clearSelectionAction =
        m_hasSelection ? menu.addAction(tr("Clear Selection")) : nullptr;

    QAction* chosen = menu.exec(ev->globalPos());
    if (!chosen) return;

    at::Points points = curve();
    if (chosen == remove && hit < int(points.size())) {
        points.erase(points.begin() + hit);
        applyPoints(points, tr("Delete Automation Point"));
    } else if (chosen == selectAllAction) {
        selectAll();
    } else if (chosen == clearSelectionAction) {
        clearSelection();
    } else if (segment >= 0 && segment < int(points.size())) {
        auto& point = points[std::size_t(segment)];
        if (chosen == linear) {
            point.shape = daw::AutomationSegment::Linear;
        } else if (chosen == hold) {
            point.shape = daw::AutomationSegment::Hold;
        } else if (chosen == scurve) {
            point.shape = daw::AutomationSegment::SCurve;
        } else if (chosen == straighten) {
            point.curve = 0.0;
        } else {
            return;
        }
        applyPoints(points, tr("Shape Automation Segment"));
    }
}

// ── AutomationEditorWindow ──────────────────────────────────────────────────

AutomationEditorWindow::AutomationEditorWindow(daw::EngineController* controller,
                                               QString trackId, QString clipId,
                                               QWidget* parent)
    : QWidget(parent), m_controller(controller),
      m_trackId(std::move(trackId)), m_clipId(std::move(clipId)) {
    setAttribute(Qt::WA_DeleteOnClose);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    buildTargetRow(outer);
    buildToolbar(outer);

    m_view = new AutomationCurveView(m_controller, this);
    m_view->setClip(m_trackId, m_clipId);
    // The toolbar is built first, so its initial combo values have not emitted
    // into the view yet. Make the visible 1/16 snap and point shape truthful
    // before the first gesture.
    m_view->setSnapBeats(m_snap->currentData().toDouble());
    m_view->setNewPointShape(
        static_cast<daw::AutomationSegment>(m_shape->currentData().toInt()));
    outer->addWidget(m_view, 1);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("PluginHint"));
    m_hint->setContentsMargins(12, 4, 12, 6);
    m_hint->setText(tr("Click to add · drag to move · Shift-point: time only · "
                       "Shift-empty: select · double-click: reset · Alt-segment: curve"));
    outer->addWidget(m_hint);

    connect(m_view, &AutomationCurveView::edited, this, [this] {
        emit projectEdited();
        updateTitle();
    });
    connect(m_view, &AutomationCurveView::liveEdited, this,
            &AutomationEditorWindow::liveEdited);
    connect(m_view, &AutomationCurveView::readoutChanged, this,
            [this](const QString& text) {
                if (m_readout) m_readout->setText(text);
            });

    reloadTargetFields();
    updateTitle();
    setMinimumSize(680, 400);
    resize(940, 520);
}

void AutomationEditorWindow::buildTargetRow(QVBoxLayout* outer) {
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("PluginWrapper"));
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(12, 8, 12, 8);
    row->setSpacing(8);

    auto* caption = new QLabel(tr("Automates"), bar);
    caption->setObjectName(QStringLiteral("PluginHint"));
    row->addWidget(caption);

    m_channel = new QComboBox(bar);
    m_channel->setMinimumWidth(150);
    m_channel->setAccessibleName(tr("Automated channel"));
    m_what = new QComboBox(bar);
    m_what->setMinimumWidth(170);
    m_what->setAccessibleName(tr("Automated control"));
    m_parameter = new QComboBox(bar);
    m_parameter->setMinimumWidth(200);
    m_parameter->setAccessibleName(tr("Automated parameter"));
    row->addWidget(m_channel);
    row->addWidget(m_what);
    row->addWidget(m_parameter);
    // An explicit stretch rather than one on the parameter field: that field is
    // hidden for everything but a plugin, and a stretch on a hidden widget
    // leaves the other two to swell across the whole window.
    row->addStretch(1);

    m_readout = new QLabel(bar);
    m_readout->setObjectName(QStringLiteral("PluginWrapperFormat"));
    m_readout->setMinimumWidth(120);
    m_readout->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(m_readout);

    // Three fields, one meaning: the target the clip carries. Any of them
    // changing re-points the curve without touching a single point of it.
    for (QComboBox* box : {m_channel, m_what, m_parameter}) {
        connect(box, &QComboBox::currentIndexChanged, this, [this] {
            if (m_reloading) return;
            applyTargetFromFields();
        });
    }
    outer->addWidget(bar);
}

void AutomationEditorWindow::buildToolbar(QVBoxLayout* outer) {
    auto* bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("PluginWrapper"));
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(12, 6, 12, 6);
    row->setSpacing(6);

    m_select = new QToolButton(bar);
    m_select->setIcon(icons::icon(icons::Glyph::Pointer, th().textPrimary));
    m_select->setToolTip(tr("Edit points"));
    m_draw = new QToolButton(bar);
    m_draw->setIcon(icons::icon(icons::Glyph::Brush, th().textPrimary));
    m_draw->setToolTip(tr("Draw freehand"));
    for (QToolButton* button : {m_select, m_draw}) {
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setFixedSize(28, 28);
        row->addWidget(button);
    }
    m_select->setChecked(true);
    connect(m_select, &QToolButton::clicked, this,
            [this] { m_view->setTool(AutomationCurveView::Tool::Select); });
    connect(m_draw, &QToolButton::clicked, this,
            [this] { m_view->setTool(AutomationCurveView::Tool::Draw); });

    row->addWidget(ui::separatorLine(Qt::Vertical, 22, bar));

    auto* snapCaption = new QLabel(tr("Snap"), bar);
    snapCaption->setObjectName(QStringLiteral("PluginHint"));
    row->addWidget(snapCaption);
    m_snap = new QComboBox(bar);
    m_snap->addItem(tr("Off"), 0.0);
    m_snap->addItem(QStringLiteral("1/16"), 0.25);
    m_snap->addItem(QStringLiteral("1/8"), 0.5);
    m_snap->addItem(QStringLiteral("1/4"), 1.0);
    m_snap->addItem(tr("1 bar"), 4.0);
    m_snap->setCurrentIndex(1);
    connect(m_snap, &QComboBox::currentIndexChanged, this, [this] {
        m_view->setSnapBeats(m_snap->currentData().toDouble());
    });
    row->addWidget(m_snap);

    auto* shapeCaption = new QLabel(tr("New point"), bar);
    shapeCaption->setObjectName(QStringLiteral("PluginHint"));
    row->addWidget(shapeCaption);
    m_shape = new QComboBox(bar);
    m_shape->addItem(tr("Linear"), int(daw::AutomationSegment::Linear));
    m_shape->addItem(tr("Hold"), int(daw::AutomationSegment::Hold));
    m_shape->addItem(tr("S-Curve"), int(daw::AutomationSegment::SCurve));
    connect(m_shape, &QComboBox::currentIndexChanged, this, [this] {
        m_view->setNewPointShape(
            daw::AutomationSegment(m_shape->currentData().toInt()));
    });
    row->addWidget(m_shape);

    row->addWidget(ui::separatorLine(Qt::Vertical, 22, bar));

    auto* lfo = new QToolButton(bar);
    lfo->setText(tr("LFO…"));
    lfo->setMinimumWidth(58);
    lfo->setToolTip(tr("Fill the selection with a shape"));
    connect(lfo, &QToolButton::clicked, this, &AutomationEditorWindow::showLfoDialog);
    row->addWidget(lfo);

    // Everything that acts on the selection, in one menu rather than eight
    // buttons: the toolbar is already the widest thing in the window, and these
    // are used one at a time.
    auto* tools = new QToolButton(bar);
    tools->setText(tr("Shape"));
    tools->setPopupMode(QToolButton::InstantPopup);
    tools->setToolButtonStyle(Qt::ToolButtonTextOnly);
    // Room for the popup arrow the style draws inside the button.
    tools->setMinimumWidth(74);
    auto* menu = new QMenu(tools);
    const auto add = [this, menu](const QString& label, const QString& undoLabel,
                                  std::function<at::Points(at::Points, at::Range)> run) {
        QAction* action = menu->addAction(label);
        connect(action, &QAction::triggered, this, [this, run, undoLabel] {
            m_view->applyPoints(run(m_view->points(), m_view->range()), undoLabel);
        });
    };
    add(tr("Invert"), tr("Invert Automation"),
        [](at::Points p, at::Range r) { return at::invert(std::move(p), r); });
    add(tr("Reverse"), tr("Reverse Automation"),
        [](at::Points p, at::Range r) { return at::reverse(std::move(p), r); });
    add(tr("Smooth"), tr("Smooth Automation"),
        [](at::Points p, at::Range r) { return at::smooth(std::move(p), 0.5, 2, r); });
    add(tr("Smooth Hard"), tr("Smooth Automation"),
        [](at::Points p, at::Range r) { return at::smooth(std::move(p), 0.8, 8, r); });
    menu->addSeparator();
    add(tr("Ramp Up"), tr("Ramp Automation"), [](at::Points p, at::Range r) {
        return at::splice(std::move(p), r, at::ramp(r.fromBeats, r.toBeats, 0.0, 1.0));
    });
    add(tr("Ramp Down"), tr("Ramp Automation"), [](at::Points p, at::Range r) {
        return at::splice(std::move(p), r, at::ramp(r.fromBeats, r.toBeats, 1.0, 0.0));
    });
    menu->addSeparator();
    add(tr("Thin Points"), tr("Thin Automation"),
        [](at::Points p, at::Range r) { return at::thin(std::move(p), 0.01, r); });
    connect(menu->addAction(tr("Quantise to Grid")), &QAction::triggered, this, [this] {
        const double grid = m_snap->currentData().toDouble();
        if (grid <= 0.0) return;
        m_view->applyPoints(
            at::quantizeTime(m_view->points(), grid, m_view->range()),
            tr("Quantise Automation"));
    });
    add(tr("Steps (4 levels)"), tr("Step Automation"),
        [](at::Points p, at::Range r) { return at::quantizeValues(std::move(p), 4, r); });
    menu->addSeparator();
    add(tr("Clear"), tr("Clear Automation"),
        [](at::Points p, at::Range r) { return at::splice(std::move(p), r, {}); });
    tools->setMenu(menu);
    row->addWidget(tools);

    row->addStretch(1);
    outer->addWidget(bar);
}

const daw::ClipModel* AutomationEditorWindow::clip() const {
    if (!m_controller) return nullptr;
    const daw::TrackModel* track =
        m_controller->project().findTrack(m_trackId.toStdString());
    if (!track) return nullptr;
    for (const daw::ClipModel& c : track->clips) {
        if (c.id == m_clipId.toStdString()) return &c;
    }
    return nullptr;
}

void AutomationEditorWindow::updateTitle() {
    const daw::ClipModel* c = clip();
    setWindowTitle(c && !c->name.empty()
                       ? tr("Automation — %1").arg(QString::fromStdString(c->name))
                       : tr("Automation"));
}

namespace {
/// The "what" field's entries. A send or a plugin slot carries its id in the
/// data, so choosing one is enough to build the target.
struct WhatEntry {
    daw::AutomationTargetKind kind = daw::AutomationTargetKind::TrackVolume;
    QString slotId;
    QString sendId;
};
}   // namespace

Q_DECLARE_METATYPE(WhatEntry)

void AutomationEditorWindow::reloadTargetFields() {
    if (!m_controller || !m_channel) return;
    const daw::ClipModel* c = clip();
    if (!c) return;
    const daw::AutomationTarget& target = c->automation.target;

    m_reloading = true;

    m_channel->clear();
    for (const daw::TrackModel& track : m_controller->project().tracks) {
        // Only tracks with a channel: a folder that does not sum, and an
        // automation lane, have nothing to drive.
        if (!daw::carriesAudio(track)) continue;
        m_channel->addItem(QString::fromStdString(track.name),
                           QString::fromStdString(track.id));
    }
    const int channelIndex =
        m_channel->findData(QString::fromStdString(target.channelId));
    if (channelIndex >= 0) m_channel->setCurrentIndex(channelIndex);

    const std::string channelId =
        m_channel->currentData().toString().toStdString();
    const daw::TrackModel* channel = m_controller->project().findTrack(channelId);

    m_what->clear();
    const auto addWhat = [this](const QString& label, WhatEntry entry) {
        m_what->addItem(label, QVariant::fromValue(entry));
        m_what->setItemData(m_what->count() - 1, int(entry.kind),
                            Qt::UserRole + 1);
    };
    addWhat(tr("Volume"), {daw::AutomationTargetKind::TrackVolume, {}, {}});
    addWhat(tr("Pan"), {daw::AutomationTargetKind::TrackPan, {}, {}});
    addWhat(tr("Mute"), {daw::AutomationTargetKind::TrackMute, {}, {}});
    if (channel) {
        for (const daw::SendModel& send : channel->sends) {
            const daw::TrackModel* bus =
                m_controller->project().findTrack(send.destinationTrackId);
            addWhat(tr("Send → %1").arg(bus ? QString::fromStdString(bus->name)
                                            : tr("Bus")),
                    {daw::AutomationTargetKind::SendLevel, {},
                     QString::fromStdString(send.id)});
        }
        if (!channel->instrument.id.empty()) {
            addWhat(tr("Instrument: %1")
                        .arg(QString::fromStdString(channel->instrument.name)),
                    {daw::AutomationTargetKind::PluginParameter, QString(), {}});
        }
        for (const daw::InsertModel& insert : channel->inserts) {
            addWhat(QString::fromStdString(insert.name),
                    {daw::AutomationTargetKind::PluginParameter,
                     QString::fromStdString(insert.id), {}});
        }
    }
    // Pick the entry that matches the clip's target.
    for (int i = 0; i < m_what->count(); ++i) {
        const WhatEntry entry = m_what->itemData(i).value<WhatEntry>();
        if (entry.kind != target.kind) continue;
        if (target.kind == daw::AutomationTargetKind::SendLevel &&
            entry.sendId.toStdString() != target.sendId)
            continue;
        if (target.kind == daw::AutomationTargetKind::PluginParameter &&
            entry.slotId.toStdString() != target.slotId)
            continue;
        m_what->setCurrentIndex(i);
        break;
    }

    const WhatEntry current = m_what->currentData().value<WhatEntry>();
    const bool plugin = current.kind == daw::AutomationTargetKind::PluginParameter;
    m_parameter->clear();
    m_parameter->setVisible(plugin);
    if (plugin && channel) {
        const std::string slotId = current.slotId.isEmpty()
                                       ? channel->instrument.id
                                       : current.slotId.toStdString();
        for (const auto& info : m_controller->insertParameters(channelId, slotId)) {
            // A parameter the plugin says cannot be automated is not offered:
            // drawing a curve into one would look like it worked and do nothing.
            if (!info.isAutomatable) continue;
            m_parameter->addItem(QString::fromStdString(info.name),
                                 QString::fromStdString(info.id));
        }
        const int parameterIndex =
            m_parameter->findData(QString::fromStdString(target.parameterId));
        if (parameterIndex >= 0) m_parameter->setCurrentIndex(parameterIndex);
    }

    m_reloading = false;
}

daw::AutomationTarget AutomationEditorWindow::targetFromFields() const {
    daw::AutomationTarget target;
    if (!m_channel || !m_what) return target;
    target.channelId = m_channel->currentData().toString().toStdString();
    const WhatEntry entry = m_what->currentData().value<WhatEntry>();
    target.kind = entry.kind;
    target.slotId = entry.slotId.toStdString();
    target.sendId = entry.sendId.toStdString();
    if (target.kind == daw::AutomationTargetKind::PluginParameter && m_parameter)
        target.parameterId = m_parameter->currentData().toString().toStdString();
    return target;
}

void AutomationEditorWindow::applyTargetFromFields() {
    if (!m_controller) return;
    const daw::AutomationTarget target = targetFromFields();
    // A plugin slot chosen but no parameter yet is a half-made target: the
    // parameter list is about to be rebuilt, and re-pointing at nothing would
    // silence the curve in between.
    if (target.kind == daw::AutomationTargetKind::PluginParameter &&
        target.parameterId.empty()) {
        reloadTargetFields();
        if (m_parameter && m_parameter->count() > 0) applyTargetFromFields();
        return;
    }
    m_controller->setAutomationTarget(m_trackId.toStdString(),
                                      m_clipId.toStdString(), target);
    reloadTargetFields();
    updateTitle();
    m_view->update();
    emit projectEdited();
}

void AutomationEditorWindow::showLfoDialog() {
    if (!m_view) return;
    const at::Range range = m_view->range();
    const at::Points before = m_view->points();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("LFO"));
    auto* form = new QFormLayout(&dialog);

    auto* wave = new QComboBox(&dialog);
    const auto waveName = [this](at::LfoWave value) {
        switch (value) {
            case at::LfoWave::Sine: return tr("Sine");
            case at::LfoWave::Triangle: return tr("Triangle");
            case at::LfoWave::Saw: return tr("Saw");
            case at::LfoWave::Ramp: return tr("Ramp");
            case at::LfoWave::Square: return tr("Square");
            case at::LfoWave::SampleHold: return tr("Sample & Hold");
        }
        return tr("Sine");
    };
    for (at::LfoWave w : at::allLfoWaves())
        wave->addItem(waveName(w), int(w));
    auto* rate = new QComboBox(&dialog);
    for (const RateChoice& choice : kRates)
        rate->addItem(QCoreApplication::translate(
                          "AutomationEditorWindow", choice.label),
                      choice.beats);
    rate->setCurrentIndex(4);
    auto* depth = new QSlider(Qt::Horizontal, &dialog);
    depth->setRange(0, 100);
    depth->setValue(100);
    auto* centre = new QSlider(Qt::Horizontal, &dialog);
    centre->setRange(0, 100);
    centre->setValue(50);
    auto* phase = new QSlider(Qt::Horizontal, &dialog);
    phase->setRange(0, 100);
    phase->setValue(0);

    form->addRow(tr("Shape"), wave);
    form->addRow(tr("Rate"), rate);
    form->addRow(tr("Depth"), depth);
    form->addRow(tr("Centre"), centre);
    form->addRow(tr("Phase"), phase);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    form->addRow(buttons);

    // Every control previews live — audibly, through the non-undoable path.
    // An LFO rate cannot be judged by looking at it.
    const auto preview = [&] {
        at::LfoSpec spec;
        spec.wave = at::LfoWave(wave->currentData().toInt());
        spec.startBeats = range.fromBeats;
        spec.lengthBeats = range.toBeats - range.fromBeats;
        spec.rateBeats = rate->currentData().toDouble();
        spec.depth = depth->value() / 100.0;
        spec.center = centre->value() / 100.0;
        spec.phase = phase->value() / 100.0;
        m_view->showPreview(at::splice(before, range, at::lfo(spec)));
    };
    connect(wave, &QComboBox::currentIndexChanged, &dialog, preview);
    connect(rate, &QComboBox::currentIndexChanged, &dialog, preview);
    for (QSlider* slider : {depth, centre, phase})
        connect(slider, &QSlider::valueChanged, &dialog, preview);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    preview();
    if (dialog.exec() == QDialog::Accepted) {
        m_view->commitPreview(tr("LFO"));
        emit projectEdited();
    } else {
        m_view->cancelPreview();
    }
}

void AutomationEditorWindow::refresh() {
    if (!clip()) {
        // The curve was deleted under the window — an undo, or the lane going
        // with its track. Editing a clip that is not there is not a state worth
        // drawing, so the window goes with it.
        close();
        return;
    }
    reloadTargetFields();
    updateTitle();
    if (m_view) m_view->update();
}

void AutomationEditorWindow::closeEvent(QCloseEvent* event) {
    emit closing(m_trackId, m_clipId);
    QWidget::closeEvent(event);
}
