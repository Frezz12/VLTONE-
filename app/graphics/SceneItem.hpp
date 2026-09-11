#pragma once
#include "SceneSnapshot.hpp"
#include <QQuickItem>

namespace ui::graphics {
class SceneItem final : public QQuickItem {
    Q_OBJECT
public:
    explicit SceneItem(QQuickItem* parent = nullptr);
    void setSnapshot(std::shared_ptr<const SceneSnapshot> snapshot);
signals:
    void resourceError(const QString& reason);
protected:
    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*) override;
private:
    std::shared_ptr<const SceneSnapshot> m_snapshot;
};
} // namespace ui::graphics
