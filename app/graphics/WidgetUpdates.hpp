#pragma once
#include <QSet>
class QWidget;
namespace ui::graphics {
// Read pending explicit QWidget updates before Qt turns them into backing-store
// Paint events, which also include unchanged children exposed by scrolling.
bool collectWidgetUpdates(QWidget* source, QSet<QWidget*>& dirty);
}
