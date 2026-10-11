#pragma once
#include <QSet>
class QWidget;
class QPoint;
namespace ui::graphics {
bool belongsToNativeOverlay(const QWidget* widget, const QWidget* source);
// Move a native editor over a retained workspace without copying/invalidation
// of the QWidget backing store beneath it. Returns false for the widget path.
bool moveNativeOverlay(QWidget* widget, const QPoint& position);
// Read pending explicit QWidget updates before Qt turns them into backing-store
// Paint events, which also include unchanged content exposed by child geometry.
// Return false when untracked backing-store damage still needs Paint events.
// Geometry exposure is already handled by retained layer transforms and clips.
bool collectWidgetUpdates(QWidget* source, QSet<QWidget*>& dirty, bool geometryExposure = false);
}
