#pragma once
#include <QQuickItem>
#include <QImage>
namespace ui::graphics {
class MediaImageItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QImage image READ image WRITE setImage NOTIFY imageChanged)
public:
    explicit MediaImageItem(QQuickItem* parent = nullptr) : QQuickItem(parent) { setFlag(ItemHasContents); }
    QImage image() const { return m_image; }
    void setImage(const QImage& image) {
        if (m_image.cacheKey() == image.cacheKey()) return;
        m_image = image; update(); emit imageChanged();
    }
signals:
    void imageChanged();
protected:
    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*) override;
private:
    QImage m_image;
};
void registerMediaImageItem();
}
