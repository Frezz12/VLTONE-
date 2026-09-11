#pragma once
#include "SceneSnapshot.hpp"
#include "SceneRecordingTag.hpp"
#include <QPaintDevice>
#include <QPaintEngine>
#include <unordered_map>

namespace ui::graphics {
struct SceneRecordingCache {
    std::unordered_map<quint64, std::vector<SceneMesh>> sections;
    QSize size;
    qreal dpr = 0;
};
// Migration adapter for existing C++ drawing routines. Records vector geometry
// and immutable assets, not a raster image of the widget. The scene owns no
// QWidget, painter or model pointer once recording has finished.
class SceneRecorder final : public QPaintDevice {
public:
    explicit SceneRecorder(QSize size, qreal dpr = 1,
                           std::shared_ptr<SceneRecordingCache> cache = {});
    ~SceneRecorder() override;
    QPaintEngine* paintEngine() const override;
    std::vector<SceneMesh> takeMeshes();
    bool supported() const;
protected:
    int metric(PaintDeviceMetric metric) const override;
private:
    class Engine;
    std::unique_ptr<Engine> m_engine;
    QSize m_size;
    qreal m_dpr;
};
} // namespace ui::graphics
