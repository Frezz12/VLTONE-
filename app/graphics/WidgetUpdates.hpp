#pragma once
#include <QSet>
class QWidget;
namespace ui::graphics {
bool belongsToNativeOverlay(const QWidget* widget, const QWidget* source);
// Read pending explicit QWidget updates before Qt turns them into backing-store
// Paint events, which also include unchanged content exposed by child geometry.
bool collectWidgetUpdates(QWidget* source, QSet<QWidget*>& dirty);
}
