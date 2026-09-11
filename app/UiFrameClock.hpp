#pragma once

#include <QObject>
#include <QPointer>
#include <QRegion>
#include <QWidget>
#include <functional>

class QWindow;

namespace ui {
enum class FrameMode { Fixed, Display, Unlimited };
class FrameTimer;

// One cadence per top-level window. Dirty requests and animation callbacks are
// coalesced before Qt paints; control, audio and network timers never join it.
class FrameClock : public QObject {
    Q_OBJECT
public:
    static FrameClock& instance();
    FrameMode mode() const { return m_mode; }
    int limit() const { return m_limit; }
    void setPreference(FrameMode mode, int limit);
    void request(QWidget* surface, const QRegion& region);
    void activate(FrameTimer* timer);
    void wake();
    double periodSeconds(const QWidget* window) const;
    // A Quick workspace supplies the window's cadence. Delivery and damage
    // callbacks run on the GUI thread before the scene graph synchronizes.
    void setPresenter(QWidget* surface, QObject* owner, QWindow* presentationWindow,
                      std::function<void()> requestFrame,
                      std::function<bool(QWidget*, const QRegion&)> damage);
    void clearPresenter(QWidget* surface, QObject* owner);
    void presentationFrame(QWidget* surface, QObject* owner);

signals:
    void preferenceChanged();

private:
    explicit FrameClock(QObject* parent);
    bool eventFilter(QObject*, QEvent*) override;
    class Driver;
    friend class Driver;
    Driver* driver(QWidget* surface);
    FrameMode m_mode = FrameMode::Display;
    int m_limit = 60;
    QList<QPointer<FrameTimer>> m_timers;
    QList<QPointer<Driver>> m_drivers;
};

// Canvas updates obey the preference; ordinary QWidget exposure and layout
// painting remain owned by Qt. Callbacks never paint synchronously.
class FrameWidget : public QWidget {
public:
    using QWidget::QWidget;
    using QWidget::update;
    void update() { FrameClock::instance().request(this, rect()); }
    void update(const QRect& region) { FrameClock::instance().request(this, region); }
    void update(const QRegion& region) { FrameClock::instance().request(this, region); }
};

class FrameTimer : public QObject {
    Q_OBJECT
public:
    explicit FrameTimer(QWidget* surface, QObject* owner = nullptr);
    void start();
    void stop();
    bool isActive() const { return m_active; }
    double deltaSeconds() const { return m_dt; }
    void setSurface(QWidget* surface);
    QWidget* surface() const { return m_surface; }
signals:
    void timeout();
private:
    friend class FrameClock;
    QPointer<QWidget> m_surface;
    bool m_active = false;
    qint64 m_lastNs = 0;
    double m_dt = 1.0 / 60.0;
};
} // namespace ui
