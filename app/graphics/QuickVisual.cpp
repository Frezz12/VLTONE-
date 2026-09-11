#include "QuickVisual.hpp"
#include "SceneRecordingTag.hpp"
#include <QHash>
#include <QCoreApplication>
#include <QThread>

namespace ui::graphics {
namespace {
QHash<quint64, QuickVisual*>& registry() { static QHash<quint64, QuickVisual*> entries; return entries; }
quint64 nextId = 0;
}
QuickVisual::QuickVisual(QObject* parent) : QObject(parent), m_id(++nextId) {
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    registry().insert(m_id, this);
}
QuickVisual::~QuickVisual() { registry().remove(m_id); }
QuickVisual* QuickVisual::find(quint64 id) { return registry().value(id); }
void QuickVisual::paintVisual(QPainter& painter, const QRectF& rect) {
    if (auto* sink = sceneGeometrySink(painter)) sink->appendVisual(m_id, rect);
}
}
