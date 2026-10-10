#include "WidgetUpdates.hpp"
#include <QWidget>
#include <QtWidgets/private/qwidget_p.h>
#include <QtWidgets/private/qwidgetrepaintmanager_p.h>

namespace ui::graphics {
namespace {
void collectSubtree(QWidget* widget, QSet<QWidget*>& dirty) {
    // A native plugin subtree is presented separately from the retained scene.
    // Do not expand its frequent editor updates into scene-recording work.
    if (widget->property("vlt.nativeOverlay").toBool()) return;
    dirty.insert(widget);
    for (auto* object : widget->children())
        if (auto* child = qobject_cast<QWidget*>(object)) collectSubtree(child, dirty);
}
}
bool belongsToNativeOverlay(const QWidget* widget, const QWidget* source) {
    for (auto* current = widget; current; current = current->parentWidget()) {
        if (current->property("vlt.nativeOverlay").toBool()) return true;
        if (current == source) break;
    }
    return false;
}
bool collectWidgetUpdates(QWidget* source, QSet<QWidget*>& dirty) {
    // Keep the version-sensitive Qt dependency here. No private state is
    // mutated: QWidget still processes its own backing-store damage normally.
    auto* manager = QWidgetPrivate::get(source)->maybeRepaintManager();
    if (!manager) return false;
    for (auto* widget : manager->dirtyWidgetList()) {
        if (widget == source || source->isAncestorOf(widget)) {
            if (belongsToNativeOverlay(widget, source)) continue;
            // An explicit parent update can affect custom child painting too.
            collectSubtree(widget, dirty);
        } else if (widget->isAncestorOf(source)) {
            collectSubtree(source, dirty);
        }
    }
    return true;
}
}
