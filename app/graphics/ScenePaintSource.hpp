#pragma once
#include <QPainter>
#include <QRegion>

namespace ui::graphics {
// The same C++ presentation routine serves QWidget exposure and Quick scene
// preparation. Quick invokes it directly, without QWidget::render, paint events
// or native backing-store work. Input and editing continue on the GUI thread.
class ScenePaintSource {
public:
    virtual ~ScenePaintSource() = default;
    virtual void paintScene(QPainter&, const QRegion&) = 0;
};
}
