#pragma once
#include "GraphicsQualityPolicy.hpp"
#include <QObject>
#include <QElapsedTimer>
#include <unordered_map>
#include <functional>
class QWidget;

namespace ui::graphics {
// Call only after application/settings paths have been established. The
// diagnostic environment override never changes the saved user preference.
bool gpuWorkspaceEnabled();
class GraphicsPreferences final : public QObject {
    Q_OBJECT
public:
    static GraphicsPreferences& instance();
    Quality quality() const { return m_policy.quality(); }
    QualityLevel level() const { return m_policy.level(); }
    double backgroundScale() const { return QualityPolicy::scale(level()); }
    int backgroundFps() const { return QualityPolicy::mediaFps(level()); }
    void setQuality(Quality quality);
    void reportFrame(const FrameStats& frame);
    void watchCompatibilityWindow(QWidget* source, std::function<void(FrameStats&)> context);
signals:
    void qualityChanged();
    void effectiveQualityChanged();
private:
    explicit GraphicsPreferences(QObject* parent);
    QualityPolicy m_policy;
    QElapsedTimer m_time;
    struct WindowFrame { FrameStats frame; qint64 at = 0; };
    std::unordered_map<std::uint64_t, WindowFrame> m_windows;
};
} // namespace ui::graphics
