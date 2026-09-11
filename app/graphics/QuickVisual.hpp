#pragma once
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQmlEngine>
#include <QPainter>

namespace ui::graphics {
// GUI-side lifetime of an embedded Quick visual. Snapshots carry only its
// opaque id; the render thread never dereferences this QObject.
class QuickVisual : public QObject {
    Q_OBJECT
public:
    explicit QuickVisual(QObject* parent = nullptr);
    ~QuickVisual() override;
    quint64 visualId() const { return m_id; }
    static QuickVisual* find(quint64 id);
    virtual QQuickItem* createItem(QQmlEngine*, QQuickItem*) = 0;
    virtual void releaseItem(QQuickItem*) {}
    virtual bool interactive() const { return false; }
    void paintVisual(QPainter&, const QRectF&);
private:
    quint64 m_id;
};
}
