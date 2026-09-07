#pragma once

#include <QLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <vector>

namespace ui {

// Larger values survive longer. This is independent of the user's tool
// profile: a control omitted by the profile is never constructed/restored.
inline void contextPriority(QWidget* widget, int priority) {
    widget->setProperty("contextPriority", priority);
}

inline void contextAvailable(QWidget* widget, bool available) {
    widget->setProperty("contextAvailable", available);
}

inline void contextContainer(QWidget* widget) {
    widget->setProperty("contextContainer", true);
}

namespace contextrow {
struct Item {
    QWidget* widget = nullptr;
    std::vector<std::unique_ptr<Item>> children;
    bool container = false;
    bool separator = false;
    bool kept = true;
    bool shown = false;
    int priority = 50;
    int width = 0;
};

inline std::unique_ptr<Item> collect(QWidget* widget, bool root,
                                      std::vector<Item*>& candidates) {
    auto item = std::make_unique<Item>();
    item->widget = widget;
    item->container = root || widget->property("contextContainer").toBool();
    item->separator = widget->property("islandSeparator").toBool();
    const QVariant available = widget->property("contextAvailable");
    item->kept = !available.isValid() || available.toBool();
    const QVariant priority = widget->property("contextPriority");
    if (priority.isValid()) item->priority = priority.toInt();
    if (item->container && widget->layout()) {
        for (int i = 0; i < widget->layout()->count(); ++i) {
            if (auto* child = widget->layout()->itemAt(i)->widget())
                item->children.push_back(collect(child, false, candidates));
        }
    } else if (item->kept && !item->separator) {
        candidates.push_back(item.get());
    }
    return item;
}

inline int measure(Item& item, bool parentKept = true) {
    item.shown = parentKept && item.kept;
    if (!item.shown) {
        for (auto& child : item.children) measure(*child, false);
        return item.width = 0;
    }
    if (!item.container) {
        const QSize wanted = item.widget->sizeHint().expandedTo(item.widget->minimumSize());
        return item.width = std::clamp(wanted.width(), 0, item.widget->maximumWidth());
    }
    for (auto& child : item.children) measure(*child);
    // Keep one divider between surviving groups, never an orphan at either
    // end. A hidden group must not leave its spacing behind.
    bool haveControl = false;
    Item* pendingDivider = nullptr;
    int width = 0;
    int count = 0;
    for (auto& child : item.children) {
        if (child->separator) {
            if (haveControl && child->shown) pendingDivider = child.get();
            child->shown = false;
            continue;
        }
        if (!child->shown || child->width == 0) continue;
        if (pendingDivider) {
            pendingDivider->shown = true;
            width += pendingDivider->width;
            ++count;
            pendingDivider = nullptr;
        }
        haveControl = true;
        width += child->width;
        ++count;
    }
    item.shown = haveControl;
    if (auto* layout = item.widget->layout(); haveControl && layout) {
        const auto margins = layout->contentsMargins();
        width += margins.left() + margins.right() + std::max(0, count - 1) * layout->spacing();
    }
    return item.width = width;
}

inline void apply(Item& item, bool root = false) {
    for (auto& child : item.children) apply(*child);
    if (!root && item.widget->isHidden() == item.shown)
        item.widget->setVisible(item.shown);
    if (item.container && item.widget->layout()) {
        item.widget->layout()->activate();
        if (!root && item.shown)
            item.widget->resize(item.width, item.widget->sizeHint().height());
    }
}
}

// Plan visibility before touching widgets. Resizing repeatedly does not
// briefly show every hidden button or reconstruct controls and their gestures.
inline int fitContextRow(QWidget* row, int availableWidth) {
    std::vector<contextrow::Item*> candidates;
    auto tree = contextrow::collect(row, true, candidates);
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto* a, const auto* b) {
        return a->priority < b->priority;
    });
    int width = contextrow::measure(*tree);
    for (auto* candidate : candidates) {
        if (width <= availableWidth) break;
        candidate->kept = false;
        width = contextrow::measure(*tree);
    }
    contextrow::apply(*tree, true);
    row->resize(width, std::max(22, row->sizeHint().height()));
    return width;
}

} // namespace ui
