#include "GraphicsPreferences.hpp"
#include <QCoreApplication>
#include <QSettings>
#include <QThread>
#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QPointer>
#include <QWidget>
#include <QWindow>
#include <QScreen>

namespace ui::graphics {
namespace {
class CompatibilityFrameMonitor : public QObject {
public:
    CompatibilityFrameMonitor(QWidget* source, std::function<void(FrameStats&)> context)
        : QObject(source), m_source(source), m_context(std::move(context)) {
        source->setProperty("vlt.compatibilityMonitor", true);
        qApp->installEventFilter(this);
        connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, this, [this] {
            if (!m_prepare.isValid()) return;
            const double elapsed = m_prepare.nsecsElapsed() / 1e6;
            m_prepare.invalidate();
            if (!m_source || !m_source->isVisible() || m_source->property("vlt.gpuSurfaceActive").toBool()) return;
            FrameStats frame;
            frame.windowId = reinterpret_cast<quintptr>(this);
            // Dispatch-to-idle GUI wall time includes QWidget backing-store
            // painting. It is a load signal, not a physical presentation time.
            frame.preparationMs = elapsed;
            frame.displayHz = m_source->screen() ? m_source->screen()->refreshRate() : 60;
            if (m_context) m_context(frame);
            GraphicsPreferences::instance().reportFrame(frame);
        });
    }
    ~CompatibilityFrameMonitor() override {
        qApp->removeEventFilter(this);
        FrameStats frame; frame.windowId = reinterpret_cast<quintptr>(this); frame.active = false;
        GraphicsPreferences::instance().reportFrame(frame);
    }
    bool eventFilter(QObject* receiver, QEvent* event) override {
        if (!m_source || m_source->property("vlt.gpuSurfaceActive").toBool()) return false;
        if (event->type() == QEvent::UpdateRequest && !m_prepare.isValid() && m_source->isVisible() &&
            (receiver == m_source->window() || receiver == m_source->window()->windowHandle())) m_prepare.start();
        if (receiver == m_source && event->type() == QEvent::Hide) {
            m_prepare.invalidate();
            FrameStats frame; frame.windowId = reinterpret_cast<quintptr>(this); frame.active = false;
            GraphicsPreferences::instance().reportFrame(frame);
        }
        return false;
    }
private:
    QPointer<QWidget> m_source;
    std::function<void(FrameStats&)> m_context;
    QElapsedTimer m_prepare;
};
}
void GraphicsPreferences::watchCompatibilityWindow(QWidget* source, std::function<void(FrameStats&)> context) {
    if (source && !source->property("vlt.compatibilityMonitor").toBool())
        new CompatibilityFrameMonitor(source, std::move(context));
}
bool gpuWorkspaceEnabled() {
    if (qEnvironmentVariableIsSet("VLT_GPU_WORKSPACE"))
        return qEnvironmentVariableIntValue("VLT_GPU_WORKSPACE") == 1;
    return QSettings().value("ui/gpuWorkspace", false).toBool();
}
GraphicsPreferences& GraphicsPreferences::instance() {
    static auto* prefs = new GraphicsPreferences(QCoreApplication::instance());
    return *prefs;
}
GraphicsPreferences::GraphicsPreferences(QObject* parent) : QObject(parent) {
    const QString saved = QSettings().value("ui/graphicsQuality", "auto").toString();
    m_policy.setQuality(saved == "maximum" ? Quality::Maximum : saved == "medium" ? Quality::Medium :
                        saved == "low" ? Quality::Low : Quality::Automatic);
    m_time.start();
}
void GraphicsPreferences::setQuality(Quality quality) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (quality == m_policy.quality()) return;
    m_policy.setQuality(quality);
    QSettings().setValue("ui/graphicsQuality", quality == Quality::Maximum ? "maximum" :
                        quality == Quality::Medium ? "medium" : quality == Quality::Low ? "low" : "auto");
    emit qualityChanged();
    emit effectiveQualityChanged();
}
void GraphicsPreferences::reportFrame(const FrameStats& frame) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto now = m_time.elapsed();
    if (frame.active) m_windows[frame.windowId] = {frame, now};
    else m_windows.erase(frame.windowId);
    FrameStats aggregate;
    aggregate.active = false;
    // Compare the busiest recently active window with its own display budget.
    // An idle detached editor must not reset the main window's overload timer.
    aggregate.frameBudgetMs = 1;
    for (auto it = m_windows.begin(); it != m_windows.end();) {
        if (now - it->second.at > 1000) { it = m_windows.erase(it); continue; }
        const auto& sample = it->second.frame;
        const double hz = std::isfinite(sample.displayHz) && sample.displayHz > 0 ? sample.displayHz : 60.;
        const double budget = sample.frameBudgetMs > 0 ? sample.frameBudgetMs : 1000. / hz;
        const double cost = std::max(sample.preparationMs + sample.synchronizationMs + sample.renderCpuMs, sample.gpuMs);
        if (std::isfinite(cost)) aggregate.preparationMs = std::max(aggregate.preparationMs, cost / budget);
        aggregate.audioXruns = std::max(aggregate.audioXruns, sample.audioXruns);
        aggregate.active = true;
        ++it;
    }
    if (m_policy.observe(aggregate, now)) emit effectiveQualityChanged();
}
} // namespace ui::graphics
