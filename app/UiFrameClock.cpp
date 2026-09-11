#include "UiFrameClock.hpp"
#include "UiFrameCadence.hpp"
#include "UiPerformance.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QWindow>
#include <algorithm>
#include <cmath>

namespace ui {
namespace {
qint64 nowNs() {
    static const QElapsedTimer epoch = [] { QElapsedTimer t; t.start(); return t; }();
    return epoch.nsecsElapsed();
}
bool visible(QWidget* surface) {
    return surface && surface->isVisible() && !surface->window()->isMinimized();
}
}
class FrameClock::Driver : public QObject {
public:
    Driver(FrameClock* clock, QWidget* window) : QObject(clock), clock(clock), window(window) {
        wait.setSingleShot(true);
        wait.setTimerType(Qt::PreciseTimer);
        connect(&wait, &QTimer::timeout, this, [this] { schedule(); });
    }
    struct Dirty { QPointer<QWidget> surface; QRegion region; };
    FrameClock* clock;
    QPointer<QWidget> window;
    QPointer<QWindow> native;
    QPointer<QObject> presenter;
    QPointer<QWindow> presentationWindow;
    std::function<void()> requestFrame;
    std::function<bool(QWidget*, const QRegion&)> presentDamage;
    quint64 generation = 0;
    QTimer wait;
    QList<Dirty> dirty;
    qint64 lastNs = 0;
    qint64 requestNs = 0;
    qint64 nativeLeadNs = 0;
    detail::FrameCadence cadence;
    bool requested = false;
    bool queued = false;

    bool needed() const {
        if (!visible(window)) return false;
        for (const auto& item : dirty) if (visible(item.surface)) return true;
        for (const auto& timer : clock->m_timers)
            if (timer && timer->m_active && visible(timer->m_surface) &&
                timer->m_surface->window() == window) return true;
        return false;
    }
    void schedule() {
        if (!window) { deleteLater(); return; }
        if (!needed()) {
            wait.stop(); requested = false; lastNs = 0; cadence.reset();
            return;
        }
        native = presenter ? presentationWindow.data() : window->windowHandle();
        if (!native || !native->isExposed()) {
            wait.stop(); requested = false; lastNs = 0; cadence.reset();
            return;
        }
        if (requested || queued) return;
        // Request ahead of the deadline by the measured platform delivery
        // latency. Waiting a whole period before requestUpdate adds another
        // display interval on a vsync platform and halves the requested rate.
        // Reserve a full display interval: the previous delivery may have
        // arrived just after requestUpdate(), which underestimates the next
        // wait for vsync. At/above the display rate, request the next vsync now.
        const double hz = window->screen() ? window->screen()->refreshRate() : 60.0;
        const auto displayPeriod = qint64(1e9 / (std::isfinite(hz) && hz >= 1 ? hz : 60.0));
        const auto remaining = cadence.deadlineNs() - nowNs() -
                              std::max(nativeLeadNs, displayPeriod);
        if (remaining > 0) {
            wait.start(int(std::max<qint64>(1, (remaining + 999999) / 1000000)));
            return;
        }
        // Leave the native UpdateRequest handler before asking for its successor.
        // QWindow provides display pacing (or its platform fallback), including
        // Unlimited mode. There is no zero-interval timer or idle paint loop.
        queued = true;
        const auto queuedAt = nowNs();
        QMetaObject::invokeMethod(this, [this, queuedAt, ticket = generation] {
            if (ticket != generation) return;
            queued = false;
            ui::perf::sample("gui.queue.delay.ms", double(nowNs() - queuedAt) / 1e6);
            if (!needed() || !native || !native->isExposed() || requested) return;
            requested = true;
            requestNs = nowNs();
            if (presenter && requestFrame) requestFrame();
            else native->requestUpdate();
        }, Qt::QueuedConnection);
    }
    void frame() {
        if (!requested) return;
        requested = false;
        const qint64 now = nowNs();
        const auto delivery = std::clamp<qint64>(now - requestNs, 0, 100000000);
        nativeLeadNs = nativeLeadNs ? (nativeLeadNs * 3 + delivery) / 4 : delivery;
        const qint64 period = qint64(clock->periodSeconds(window) * 1e9);
        if (!cadence.due(now, period)) { schedule(); return; }
        cadence.advance(now, period);
        if (lastNs) {
            ui::perf::sample("frame.interval.ms", double(now - lastNs) / 1e6);
            if (now - lastNs >= 250000000)
                ui::perf::sample("frame.stall.ms", double(now - lastNs) / 1e6);
        }
        lastNs = now;
        ui::perf::Scope timing("frame.prepare.ms");
        const auto timers = clock->m_timers; // callbacks may destroy/register timers
        for (const auto& timer : timers) {
            if (!timer || !timer->m_active || !visible(timer->m_surface) ||
                timer->m_surface->window() != window) continue;
            timer->m_dt = timer->m_lastNs ? double(now - timer->m_lastNs) / 1e9 : 1.0 / 60.0;
            timer->m_lastNs = now;
            emit timer->timeout();
        }
        auto paint = std::move(dirty);
        dirty.clear();
        for (const auto& item : paint)
            if (visible(item.surface) && !(presenter && presentDamage && presentDamage(item.surface, item.region)))
                item.surface->update(item.region);
        schedule();
    }
    void resetPresenter() {
        ++generation;
        presenter = nullptr; presentationWindow = nullptr;
        requestFrame = {}; presentDamage = {};
        native = nullptr; requested = queued = false;
        wait.stop(); lastNs = nativeLeadNs = 0; cadence.reset();
    }
};

FrameClock& FrameClock::instance() {
    static FrameClock* clock = new FrameClock(qApp);
    return *clock;
}
FrameClock::FrameClock(QObject* parent) : QObject(parent) {
    QSettings settings;
    const auto mode = settings.value("ui/frameMode", "display").toString();
    m_mode = mode == "display" ? FrameMode::Display : mode == "unlimited" ? FrameMode::Unlimited : FrameMode::Fixed;
    m_limit = std::clamp(settings.value("ui/frameLimit", 60).toInt(), 1, 1000);
    qApp->installEventFilter(this);
}
void FrameClock::setPreference(FrameMode mode, int limit) {
    limit = std::clamp(limit, 1, 1000);
    if (m_mode == mode && m_limit == limit) return;
    m_mode = mode; m_limit = limit;
    QSettings settings;
    settings.setValue("ui/frameMode", mode == FrameMode::Display ? "display" : mode == FrameMode::Unlimited ? "unlimited" : "fixed");
    settings.setValue("ui/frameLimit", limit);
    for (const auto& driver : m_drivers) if (driver) {
        driver->lastNs = 0;
        driver->cadence.reset();
        driver->wait.stop();
    }
    emit preferenceChanged();
    wake();
}
double FrameClock::periodSeconds(const QWidget* window) const {
    if (m_mode == FrameMode::Unlimited) return 0.0;
    double hz = m_limit;
    if (m_mode == FrameMode::Display) {
        const auto* screen = window ? window->screen() : nullptr;
        hz = screen ? screen->refreshRate() : 60.0;
        if (!std::isfinite(hz) || hz < 1) hz = 60.0;
    }
    return 1.0 / hz;
}
FrameClock::Driver* FrameClock::driver(QWidget* surface) {
    if (!surface) return nullptr;
    auto* window = surface->window();
    for (const auto& d : m_drivers) if (d && d->window == window) return d;
    auto* d = new Driver(this, window);
    m_drivers.push_back(d);
    return d;
}
void FrameClock::setPresenter(QWidget* surface, QObject* owner, QWindow* window,
                              std::function<void()> requestFrame,
                              std::function<bool(QWidget*, const QRegion&)> damage) {
    auto* d = driver(surface);
    if (!d || !owner || !window) return;
    d->resetPresenter();
    d->presenter = owner; d->presentationWindow = window;
    d->requestFrame = std::move(requestFrame); d->presentDamage = std::move(damage);
    connect(owner, &QObject::destroyed, d, [d, ticket = d->generation] {
        if (d->generation != ticket) return;
        d->resetPresenter(); d->schedule();
    });
    d->schedule();
}
void FrameClock::clearPresenter(QWidget* surface, QObject* owner) {
    for (const auto& d : std::as_const(m_drivers))
        if (d && surface && d->window == surface->window() && d->presenter == owner) {
            d->resetPresenter(); d->schedule();
        }
}
void FrameClock::presentationFrame(QWidget* surface, QObject* owner) {
    for (const auto& d : std::as_const(m_drivers))
        if (d && surface && d->window == surface->window() && d->presenter == owner) d->frame();
}
void FrameClock::request(QWidget* surface, const QRegion& region) {
    if (!surface || region.isEmpty() || !visible(surface)) return;
    auto* d = driver(surface);
    for (auto& item : d->dirty) if (item.surface == surface) {
        item.region += region;
        if (item.region.rectCount() > 64) item.region = item.region.boundingRect();
        d->schedule(); return;
    }
    d->dirty.push_back({surface, region});
    d->schedule();
}
void FrameClock::activate(FrameTimer* timer) {
    if (!m_timers.contains(timer)) m_timers.push_back(timer);
    wake();
}
void FrameClock::wake() {
    m_timers.removeIf([](const auto& t) { return !t; });
    m_drivers.removeIf([](const auto& d) { return !d; });
    for (const auto& timer : std::as_const(m_timers))
        if (timer && timer->m_active && timer->m_surface) driver(timer->m_surface);
    for (const auto& d : std::as_const(m_drivers)) if (d) d->schedule();
}
bool FrameClock::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::UpdateRequest) {
        for (const auto& d : std::as_const(m_drivers)) {
            if (d && !d->presenter && d->native == object && d->requested) {
                d->frame();
                // This native request is our cadence pulse. QWidgetWindow's
                // default handler calls repaint() on the entire top-level
                // widget, discarding every narrow dirty region prepared above.
                // Consume only our own pulse; QWidget::update() still queues
                // the ordinary backing-store update, and exposures/requests
                // from other callers retain Qt's normal handling.
                return true;
            }
        }
    } else if (event->type() == QEvent::Show || event->type() == QEvent::Hide ||
               event->type() == QEvent::Expose || event->type() == QEvent::WindowStateChange ||
               event->type() == QEvent::ScreenChangeInternal) {
        const auto* widget = qobject_cast<QWidget*>(object);
        bool relevant = qobject_cast<QWindow*>(object) || (widget && widget->isWindow());
        if (widget && !relevant) {
            for (const auto& timer : std::as_const(m_timers)) {
                if (timer && timer->m_active && timer->m_surface &&
                    (widget == timer->m_surface || widget->isAncestorOf(timer->m_surface))) {
                    relevant = true;
                    break;
                }
            }
        }
        if (relevant) wake();
    }
    return false;
}
FrameTimer::FrameTimer(QWidget* surface, QObject* owner)
    : QObject(owner ? owner : surface), m_surface(surface) {}
void FrameTimer::start() {
    if (m_active) return;
    m_active = true; m_lastNs = 0;
    FrameClock::instance().activate(this);
}
void FrameTimer::stop() { m_active = false; m_lastNs = 0; }
void FrameTimer::setSurface(QWidget* surface) {
    if (m_surface == surface) return;
    m_surface = surface; m_lastNs = 0;
    if (m_active) FrameClock::instance().wake();
}
} // namespace ui
