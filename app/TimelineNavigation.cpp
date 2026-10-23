#include "TimelineNavigation.hpp"
#include "TimelineNavigationPaint.hpp"
#include "ScrollMotion.hpp"
#include "Theme.hpp"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScopedValueRollback>
#include <algorithm>
#include <cmath>

namespace {
constexpr double coastSeconds = .15;
constexpr qint64 velocityWindowNs = 80000000;
double coordinate(const QPointF& p, Qt::Orientation axis) {
    return axis == Qt::Horizontal ? p.x() : p.y();
}
}

TimelineNavigationRail::TimelineNavigationRail(Qt::Orientation axis, QWidget* parent)
    : QScrollBar(axis, parent), m_coast(this), m_coastEase(QEasingCurve::BezierSpline) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoMousePropagation);
    setMouseTracking(true);
    setFocusPolicy(Qt::TabFocus);
    setCursor(axis == Qt::Horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);
    parent->installEventFilter(this);
    // Same short, non-bouncing ease-out as the application's scroll motion.
    m_coastEase.addCubicBezierSegment({.23, 1.}, {.32, 1.}, {1., 1.});
    connect(&m_coast, &ui::FrameTimer::timeout, this, [this] {
        const double progress = std::min(1., m_coastClock.nsecsElapsed() / (coastSeconds * 1e9));
        applyValue(m_coastFrom + (m_coastTo - m_coastFrom) * m_coastEase.valueForProgress(progress));
        if (progress >= 1. || value() == minimum() || value() == maximum()) m_coast.stop();
    });
    connect(&ui::ScrollPreferences::instance(), &ui::ScrollPreferences::changed, this, [this] {
        if (!ui::ScrollPreferences::instance().effectiveEnabled()) m_coast.stop();
    });
    connect(this, &QScrollBar::sliderPressed, this, [this] {
        ui::FrameClock::instance().request(this, rect());
    });
    connect(this, &QScrollBar::sliderReleased, this, [this] {
        ui::FrameClock::instance().request(this, rect());
    });
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] {
        ui::FrameClock::instance().request(this, rect());
    });
}

void TimelineNavigationRail::sliderChange(SliderChange change) {
    if (!m_applyingValue || change != SliderValueChange) m_coast.stop();
    // QScrollBar's default implementation repaints at input frequency. These
    // controls share the arrangement's cadence instead.
    ui::FrameClock::instance().request(this, rect());
}

void TimelineNavigationRail::setPlayheadPosition(double x) {
    if (orientation() != Qt::Horizontal || m_playheadX == x) return;
    const auto marker = [this](double at) {
        return std::isfinite(at) && at >= 0 && at < width()
            ? QRect(int(std::floor(at)) - 2, 0, 5, height()).intersected(rect()) : QRect();
    };
    const QRegion dirty = QRegion(marker(m_playheadX)) + marker(x);
    m_playheadX = x;
    if (!dirty.isEmpty()) ui::FrameClock::instance().request(this, dirty);
}

void TimelineNavigationRail::setLeadingConnected(bool connected) {
    if (m_leadingConnected == connected) return;
    m_leadingConnected = connected;
    ui::FrameClock::instance().request(this, rect());
}

void TimelineNavigationRail::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const bool horizontal = orientation() == Qt::Horizontal;
    ui::navigation::paintRailSurface(p, rect(), orientation(), !m_leadingConnected);
    const double length = horizontal ? width() : height();
    const double breadth = horizontal ? height() : width();
    if (length <= 0 || breadth <= 0) return;
    if (!horizontal) { p.translate(width(), 0); p.rotate(90); }
    constexpr double spacing = 7;
    // Horizontal ticks follow the hand while the viewport advances in time.
    const double phase = (horizontal ? -1. : 1.) * double(value()) / std::max(1, pageStep()) * length;
    const double first = std::floor((phase - length * .5) / spacing);
    const bool active = maximum() > minimum();
    p.setPen(Qt::NoPen);
    for (double i = first; ; ++i) {
        const double x = length * .5 + i * spacing - phase;
        if (x > length) break;
        if (x < 0) continue;
        const double edge = std::clamp(std::min(x, length - x) / 30., 0., 1.);
        const bool major = std::fmod(std::abs(i), 5.) < .5;
        auto color = th().textPrimary;
        color.setAlphaF(edge * (active ? (major ? .30 : .16) : .08));
        const double tickHeight = major ? 7 : 4;
        p.setBrush(color);
        p.drawRoundedRect(QRectF(x - 1, (breadth - tickHeight) * .5, 2, tickHeight), 1, 1);
    }
    if (horizontal && std::isfinite(m_playheadX) && m_playheadX >= 0 && m_playheadX < length) {
        auto accent = th().accent;
        accent.setAlphaF(.85);
        p.setPen(QPen(accent, 2, Qt::SolidLine, Qt::RoundCap));
        const double x = std::clamp(m_playheadX, 1., std::max(1., length - 1.));
        p.drawLine(QPointF(x, 4), QPointF(x, breadth - 4));
    }
    if (underMouse() || isSliderDown() || hasFocus()) {
        auto light = th().textPrimary; light.setAlphaF(isSliderDown() ? .045 : .025);
        p.setPen(Qt::NoPen);
        p.setBrush(light);
        p.drawRoundedRect(QRectF(2, 2, length - 4, breadth - 4), 5, 5);
    }
    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(th().accent, 1, Qt::DotLine));
        p.drawRoundedRect(QRectF(2.5, 2.5, length - 5, breadth - 5), 4, 4);
    }
}

void TimelineNavigationRail::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) { event->ignore(); return; }
    cancelInteraction();
    emit interactionStarted();
    m_lastPointer = coordinate(event->position(), orientation());
    m_dragValue = value();
    m_unitsPerPixel = double(pageStep()) / std::max(1, orientation() == Qt::Horizontal ? width() : height());
    m_lastDelta = 0;
    m_dragClock.start();
    m_samples.push_back({0, m_dragValue});
    setSliderDown(true);
    event->accept();
}
void TimelineNavigationRail::dragTo(const QPointF& point) {
    const double at = coordinate(point, orientation());
    const double before = m_dragValue;
    // Consume every delta even at a boundary: reversing must never travel
    // back through an accumulated out-of-range distance.
    m_dragValue = std::clamp(m_dragValue + (at - m_lastPointer) * m_unitsPerPixel,
                             double(minimum()), double(maximum()));
    if (at != m_lastPointer) {
        const qint64 now = m_dragClock.nsecsElapsed();
        const double delta = m_dragValue - before;
        if (delta == 0 || delta * m_lastDelta < 0) {
            const qint64 previous = m_samples.empty() ? now : m_samples.back().ns;
            m_samples.clear();
            m_samples.push_back({previous, before});
        }
        m_samples.push_back({now, m_dragValue});
        while (m_samples.size() > 2 &&
               (m_samples.size() > 12 || now - m_samples.front().ns > velocityWindowNs))
            m_samples.pop_front();
        m_lastDelta = delta;
    }
    m_lastPointer = at;
    applyValue(m_dragValue);
}
void TimelineNavigationRail::applyValue(double value) {
    const QScopedValueRollback applying(m_applyingValue, true);
    setValue(int(std::lround(value)));
}
void TimelineNavigationRail::finishDrag() {
    setSliderDown(false);
    if (!ui::ScrollPreferences::instance().effectiveEnabled() || m_samples.size() < 2) return;
    const auto first = m_samples.front(), last = m_samples.back();
    if (last.ns <= first.ns || m_dragClock.nsecsElapsed() - last.ns > velocityWindowNs) return;
    const double velocity = (last.value - first.value) * 1e9 / (last.ns - first.ns);
    // Match the initial slope of the shared easing, with at most 12 pixels of
    // extra travel. A pause before release or a boundary produces no throw.
    const double limit = std::abs(m_unitsPerPixel) * 12.;
    const double distance = std::clamp(velocity * coastSeconds * .23, -limit, limit);
    m_coastFrom = value();
    m_coastTo = std::clamp(m_coastFrom + distance, double(minimum()), double(maximum()));
    if (std::abs(m_coastTo - m_coastFrom) < .5) return;
    m_coastClock.start();
    m_coast.start();
}
void TimelineNavigationRail::cancelInteraction() {
    m_coast.stop();
    m_samples.clear();
    setSliderDown(false);
}
void TimelineNavigationRail::mouseMoveEvent(QMouseEvent* event) {
    if (isSliderDown()) {
        if (event->buttons() & Qt::LeftButton) dragTo(event->position());
        else cancelInteraction();
    }
    event->accept();
}
void TimelineNavigationRail::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isSliderDown()) { event->ignore(); return; }
    dragTo(event->position());
    finishDrag();
    event->accept();
}
void TimelineNavigationRail::keyPressEvent(QKeyEvent* event) {
    cancelInteraction();
    if (event->key() == Qt::Key_Escape) {
        event->accept(); return;
    }
    emit interactionStarted();
    QScrollBar::keyPressEvent(event);
}
bool TimelineNavigationRail::event(QEvent* event) {
    if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::Resize || event->type() == QEvent::Wheel ||
        (event->type() == QEvent::UngrabMouse && isSliderDown())) {
        cancelInteraction();
    }
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave)
        ui::FrameClock::instance().request(this, rect());
    return QScrollBar::event(event);
}
bool TimelineNavigationRail::eventFilter(QObject*, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::KeyPress ||
        event->type() == QEvent::Wheel || event->type() == QEvent::NativeGesture ||
        event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)
        cancelInteraction();
    return false;
}

TimelineNavigationCorner::TimelineNavigationCorner(QWidget* parent) : FrameWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoMousePropagation);
    connect(&ThemeManager::instance(), &ThemeManager::changed, this, [this] { update(); });
}
void TimelineNavigationCorner::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), ui::navigation::railBase());
    // Join both inset wells without a separate border or two end caps.
    QPainterPath joint;
    joint.moveTo(2, -1);
    joint.lineTo(width() - 2, -1);
    joint.lineTo(width() - 2, height() - 7);
    joint.quadTo(width() - 2, height() - 2, width() - 7, height() - 2);
    joint.lineTo(-1, height() - 2);
    joint.lineTo(-1, 2);
    joint.lineTo(0, 2);
    joint.quadTo(2, 2, 2, 0);
    joint.closeSubpath();
    QRadialGradient depth(QPointF(0, 0), width());
    ui::navigation::setWellDepth(depth);
    p.fillPath(joint, depth);
}
