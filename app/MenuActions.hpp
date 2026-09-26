#pragma once

#include <QMenu>
#include <functional>

namespace ui {
inline void clearMenu(QMenu& menu) {
    qDeleteAll(menu.findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly));
    menu.clear();
}

// Context targets can differ from the global selection. Show its key hint
// without registering a second shortcut in a persistent menu-bar submenu.
inline void showCommandShortcut(QAction* action, const QAction* command) {
    if (command && !command->shortcut().isEmpty())
        action->setText(action->text() + QLatin1Char('\t') +
                        command->shortcut().toString(QKeySequence::NativeText));
}

// Bind the current leaves, not QMenu::triggered: repopulating a persistent
// menu must also discard its old handlers. Each action owns its connection.
inline void connectMenuActions(QMenu& menu, QObject* receiver,
                               const std::function<void(QAction*)>& run) {
    for (QAction* action : menu.actions()) {
        if (action->menu()) {
            connectMenuActions(*action->menu(), receiver, run);
        } else if (!action->isSeparator()) {
            QObject::connect(action, &QAction::triggered, receiver,
                             [action, run] { run(action); });
        }
    }
}
} // namespace ui
