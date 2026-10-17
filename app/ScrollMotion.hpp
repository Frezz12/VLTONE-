#pragma once
#include "UiFrameClock.hpp"
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QPointF>
#include <functional>

namespace ui {
// One preference for native lists and the custom editing canvases.
class ScrollPreferences final : public QObject {
    Q_OBJECT
public:
    static ScrollPreferences& instance();
    bool enabled() const { return m_enabled; }
    bool reducedMotion() const { return m_reduced; }
    bool effectiveEnabled() const { return m_enabled && !m_reduced && m_strength > 0; }
    int speed() const { return m_speed; }
    int strength() const { return m_strength; }
    void setEnabled(bool);
    void setReducedMotion(bool);
    void setSpeed(int);
    void setStrength(int);
signals:
    void changed();
private:
    ScrollPreferences();
    bool m_enabled = true, m_reduced = false;
    int m_speed = 100, m_strength = 65;
};

// Positions are viewport pixels. The callbacks own the normal bounds and
// notification paths, so animation never creates a second scroll position.
class ScrollMotion final : public QObject {
    Q_OBJECT
public:
    using Read = std::function<QPointF()>;
    using Write = std::function<void(QPointF)>;
    using Constrain = std::function<QPointF(QPointF)>;
    static void install();
    static void scroll(QWidget* owner, QPointF delta, bool precise, Read read, Write write);
    // Short interpolation for grab navigation. Bounds apply to the exact
    // accumulated destination, independently of delivered presentation frames.
    static void pan(QWidget* owner, QPointF delta, Read read, Write write, Constrain constrain);
    static void finishPan(QWidget* owner);
    static void cancel(QWidget* owner);
    // Scales are positive factors, interpolated in log space on the same
    // frame clock and preferences as scrolling. The writer owns the anchor.
    static void zoom(QWidget* owner, QPointF factors, bool precise, Read read, Write write);
    static void cancelZoom(QWidget* owner);
    static bool isZooming(QWidget* owner);
private:
    ScrollMotion(QWidget* owner, Read read, Write write, bool logarithmic = false);
    void move(QPointF delta, bool precise);
    void movePan(QPointF delta, const Constrain& constrain);
    double progress() const;
    void advance();
    void apply(QPointF position);
    void stop();
    bool eventFilter(QObject*, QEvent*) override;
    Read m_read;
    Write m_write;
    FrameTimer m_frames;
    QEasingCurve m_easing;
    QElapsedTimer m_motionClock;
    QPointF m_from, m_position, m_target, m_actual;
    double m_duration = .15;
    bool m_initialized = false;
    bool m_logarithmic = false;
    bool m_pan = false;
};
} // namespace ui
