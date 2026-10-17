#include "WidgetUpdates.hpp"
#include <QWidget>
#include <QWindow>
#include <QVariant>
#include <QCoreApplication>
#include <QMoveEvent>
#include <QtWidgets/private/qwidget_p.h>
#include <QtWidgets/private/qwidgetrepaintmanager_p.h>

namespace ui::graphics {
namespace {
void collectSubtree(QWidget* widget, const QRegion& region, QSet<QWidget*>& dirty) {
    // A native plugin subtree is presented separately from the retained scene.
    // Do not expand its frequent editor updates into scene-recording work.
    if (region.isEmpty() || !widget->isVisible() || widget->property("vlt.nativeOverlay").toBool()) return;
    dirty.insert(widget);
    for (auto* object : widget->children())
        if (auto* child = qobject_cast<QWidget*>(object)) {
            const auto damage = region.intersected(child->geometry()).translated(-child->pos());
            collectSubtree(child, damage, dirty);
        }
}
}
bool belongsToNativeOverlay(const QWidget* widget, const QWidget* source) {
    for (auto* current = widget; current; current = current->parentWidget()) {
        if (current->property("vlt.nativeOverlay").toBool()) return true;
        if (current == source) break;
    }
    return false;
}
void raiseNativeOverlay(QWidget* widget) {
    if (auto* chrome = widget->property("vlt.nativeOverlayChrome").value<QWindow*>();
        chrome && chrome->isVisible()) chrome->raise();
    widget->raise();
    if (widget->windowHandle()) widget->windowHandle()->raise();
}
bool moveNativeOverlay(QWidget* widget, const QPoint& position) {
    if (!widget || widget->isWindow() || !widget->isVisible() || !widget->windowHandle() ||
        !widget->property("vlt.nativeOverlay").toBool()) return false;
    for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
        if (parent->property("vlt.gpuSurfaceActive").toBool()) {
            auto* nativeParent = widget->nativeParentWidget();
            if (!nativeParent) return false;
            // QWidget::move also copies backing-store pixels and marks exposed
            // parents dirty. The GPU scene already retains those pixels. Qt's
            // native move event keeps QWidget geometry and input mapping in sync.
            auto* window = widget->windowHandle();
            const QPoint oldPosition = window->position();
            window->setPosition(widget->parentWidget()->mapTo(nativeParent, position));
            // Some platforms queue that event. Deliver the geometry change
            // before release/save or another input reads QWidget coordinates.
            if (widget->pos() != position) {
                QMoveEvent moved(window->position(), oldPosition);
                QCoreApplication::sendEvent(window, &moved);
            }
            // The native surface already retains its pixels. Repainting here
            // redraws the entire editor subtree for every pointer sample.
            // Actual control/content changes still use ordinary QWidget damage.
            return true;
        }
        if (parent->isWindow()) break;
    }
    return false;
}
bool collectWidgetUpdates(QWidget* source, QSet<QWidget*>& dirty, bool geometryExposure) {
    // Keep the version-sensitive Qt dependency here. No private state is
    // mutated: QWidget still processes its own backing-store damage normally.
    auto* manager = QWidgetPrivate::get(source)->maybeRepaintManager();
    if (!manager) return false;
    for (auto* widget : manager->dirtyWidgetList()) {
        const auto pending = QWidgetPrivate::get(widget)->dirty;
        // Empty private damage means Qt only supplied an invalidation flag.
        // Preserve the full fallback in that case; otherwise keep the actual
        // disjoint rectangles instead of dirtying every descendant.
        const QRegion region = pending.isEmpty() ? QRegion(widget->rect()) : pending;
        if (widget == source || source->isAncestorOf(widget)) {
            if (belongsToNativeOverlay(widget, source)) continue;
            // An explicit parent update can affect custom child painting too.
            collectSubtree(widget, region, dirty);
        } else if (widget->isAncestorOf(source)) {
            const auto origin = source->mapTo(widget, QPoint());
            collectSubtree(source, region.translated(-origin).intersected(source->rect()), dirty);
        }
    }
    // QWidget::scroll() can expose changed content without a dirty widget.
    // Keep Qt's Paint fallback for untracked damage instead of invalidating
    // the entire subtree (including layers Qt would not actually paint).
    if (geometryExposure) return true;
    const auto origin = source->mapTo(source->window(), QPoint());
    return manager->dirtyRegion().translated(-origin).intersected(source->rect()).isEmpty();
}
}
