#include "MediaImageItem.hpp"
#include "VideoFrameGate.hpp"
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QtQml>
namespace ui::graphics {
namespace {
struct ImageNode : QSGSimpleTextureNode { qint64 key = 0; };
}
QSGNode* MediaImageItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    if (m_image.isNull()) { delete old; return nullptr; }
    auto* node = old ? static_cast<ImageNode*>(old) : new ImageNode;
    if (node->key != m_image.cacheKey()) {
        auto* texture = window()->createTextureFromImage(m_image);
        if (!texture) { delete node; return nullptr; }
        // QSGSimpleTextureNode releases its previous texture from setTexture()
        // when ownsTexture is true. Deleting the saved pointer again crashed
        // the render thread whenever a live background image was replaced.
        node->setTexture(texture);
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
        node->key = m_image.cacheKey();
    }
    node->setRect(boundingRect());
    return node;
}
void registerMediaImageItem() {
    static const int type = qmlRegisterType<MediaImageItem>("Vlt.Graphics", 1, 0, "MediaImage");
    Q_UNUSED(type);
    static const int gate = qmlRegisterType<VideoFrameGate>("Vlt.Graphics", 1, 0, "VideoFrameGate");
    Q_UNUSED(gate);
}
}
