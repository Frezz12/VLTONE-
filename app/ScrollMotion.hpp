#pragma once
#include "UiFrameClock.hpp"
#include <QEasingCurve>
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
    static void install();
    static void scroll(QWidget* owner, QPointF delta, bool precise, Read read, Write write);
    static void cancel(QWidget* owner);
private:
    ScrollMotion(QWidget* owner, Read read, Write write);
    void move(QPointF delta, bool precise);
    void advance();
    void apply(QPointF position);
    void stop();
    bool eventFilter(QObject*, QEvent*) override;
    Read m_read;
    Write m_write;
    FrameTimer m_frames;
    QEasingCurve m_easing;
    QPointF m_from, m_position, m_target, m_actual;
    double m_elapsed = 0.0, m_duration = .15;
    bool m_initialized = false;
};
} // namespace ui
