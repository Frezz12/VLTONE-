#pragma once
#include <QCursor>
#include <QGuiApplication>
#include <QPointF>
#include <QScreen>

namespace ui {
/// Relative pointer gesture for controls that must keep moving after the
/// physical pointer would otherwise reach a screen edge. Track successive
/// floating-point positions; recenter only at an edge and ignore the warp's
/// queued events, so mouse and trackpad samples cover the same distance.
class LockedCursorDrag {
public:
    void begin(const QPointF& globalPosition);
    QPointF takeDelta(const QPointF& globalPosition, bool wrapAtEdge = true);
    QPointF finish(const QPointF& globalPosition);
    void cancel();
    bool active() const { return m_active; }

private:
    QPointF m_anchor;
    QPointF m_lastPosition;
    QPointF m_warpFrom;
    bool m_warpPending = false;
    bool m_active = false;
};

inline void LockedCursorDrag::begin(const QPointF& globalPosition) {
    m_anchor = m_lastPosition = globalPosition;
    m_warpPending = false;
    m_active = true;
}

inline QPointF LockedCursorDrag::takeDelta(const QPointF& globalPosition, bool wrapAtEdge) {
    if (!m_active) return {};
    if (m_warpPending) {
        // Native input can already have several old-edge samples queued when
        // setPos runs. Wait for the destination instead of counting them again.
        if ((globalPosition - m_lastPosition).manhattanLength() >
            (globalPosition - m_warpFrom).manhattanLength()) return {};
        m_warpPending = false;
    }
    const QPointF delta = globalPosition - m_lastPosition;
    m_lastPosition = globalPosition;
    if (wrapAtEdge && !delta.isNull()) {
        if (const auto* screen = QGuiApplication::screenAt(globalPosition.toPoint())) {
            const QRect bounds = screen->geometry();
            QPointF destination = globalPosition;
            if ((delta.x() < 0 && globalPosition.x() <= bounds.left() + 2) ||
                (delta.x() > 0 && globalPosition.x() >= bounds.right() - 2))
                destination.setX(bounds.center().x());
            if ((delta.y() < 0 && globalPosition.y() <= bounds.top() + 2) ||
                (delta.y() > 0 && globalPosition.y() >= bounds.bottom() - 2))
                destination.setY(bounds.center().y());
            if (destination != globalPosition) {
                m_warpFrom = globalPosition;
                m_lastPosition = destination.toPoint();
                m_warpPending = true;
                QCursor::setPos(m_lastPosition.toPoint());
                // A backend without pointer warping must keep ordinary input
                // working instead of waiting for a move that cannot arrive.
                if (QCursor::pos() != m_lastPosition.toPoint()) {
                    m_lastPosition = globalPosition;
                    m_warpPending = false;
                }
            }
        }
    }
    return delta;
}

inline QPointF LockedCursorDrag::finish(const QPointF& globalPosition) {
    if (!m_active) return {};
    const QPointF delta = takeDelta(globalPosition, false);
    m_active = false;
    m_warpPending = false;
    if (QCursor::pos() != m_anchor.toPoint()) QCursor::setPos(m_anchor.toPoint());
    return delta;
}

inline void LockedCursorDrag::cancel() {
    m_active = false;
    m_warpPending = false;
}

} // namespace ui
