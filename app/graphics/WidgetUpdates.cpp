#include "WidgetUpdates.hpp"
#include <QWidget>
#include <QtWidgets/private/qwidget_p.h>
#include <QtWidgets/private/qwidgetrepaintmanager_p.h>

namespace ui::graphics {
bool collectWidgetUpdates(QWidget* source, QSet<QWidget*>& dirty) {
    // Keep the version-sensitive Qt dependency here. No private state is
    // mutated: QWidget still processes its own backing-store damage normally.
    auto* manager = QWidgetPrivate::get(source)->maybeRepaintManager();
    if (!manager) return false;
    for (auto* widget : manager->dirtyWidgetList()) {
        if (widget == source || source->isAncestorOf(widget)) {
            dirty.insert(widget);
            // An explicit parent update can affect custom child painting too.
            for (auto* child : widget->findChildren<QWidget*>()) dirty.insert(child);
        } else if (widget->isAncestorOf(source)) {
            dirty.insert(source);
            for (auto* child : source->findChildren<QWidget*>()) dirty.insert(child);
        }
    }
    return true;
}
}
