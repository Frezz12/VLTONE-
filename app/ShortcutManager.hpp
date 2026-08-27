#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

class QAction;
class QKeyEvent;

/// The single source of truth for user commands and their keyboard shortcuts.
/// Every menu item / rebindable action registers here with a stable id, a
/// human label, a category (for grouping in the settings UI) and a default key.
/// Saved overrides live in QSettings under `shortcuts/<id>`, so a user's custom
/// keys survive restarts. Changing a shortcut updates the live QAction, so it
/// takes effect immediately with no restart.
class ShortcutManager : public QObject {
    Q_OBJECT
public:
    struct Command {
        QString id;
        QString label;
        QString category;
        QKeySequence defaultSeq;
        QAction* action = nullptr;   // the live action this command drives
    };

    explicit ShortcutManager(QObject* parent = nullptr);

    /// Register `action` under `id`; applies the saved-or-default shortcut to it.
    void registerCommand(const QString& id, const QString& label,
                         const QString& category, const QKeySequence& def,
                         QAction* action);

    const QVector<Command>& commands() const { return m_commands; }

    /// Take a set of keys away from the actions, without changing what the
    /// commands are *bound* to.
    ///
    /// The typing keyboard needs the bare letters for notes, and on macOS it is
    /// not enough to swallow the key event: the menu bar is the system's, and
    /// AppKit fires a key equivalent before any Qt filter is consulted. So the
    /// binding is lifted off the QAction while the keyboard is on — but it is
    /// still the binding, and everything that answers "what is this bound to?"
    /// (`shortcut`, `conflict`, the settings page built on them) keeps
    /// answering with it rather than with the emptied action. Pass an empty
    /// function to give the keys back.
    void setKeySuppressor(std::function<bool(int key)> suppressed);

    /// What the command is bound to ({} when unbound/unknown). This is the
    /// binding, which is not always what its action currently carries — see
    /// setKeySuppressor.
    QKeySequence shortcut(const QString& id) const;
    QKeySequence defaultShortcut(const QString& id) const;
    /// Public for the shortcut editor: turn a key captured under a non-Latin
    /// layout into the physical Latin-position binding shown in menus.
    QKeySequence canonicalShortcut(const QKeySequence& seq) const {
        return canonicalSequence(seq);
    }

    /// Id of another command already bound to `seq` (excluding `id`), or "" when
    /// free. An empty sequence never conflicts.
    QString conflict(const QString& id, const QKeySequence& seq) const;
    /// Label for a command id, for messages.
    QString label(const QString& id) const;

    /// Apply `seq` to the command and persist it. An empty sequence unbinds it.
    void setShortcut(const QString& id, const QKeySequence& seq);
    void resetToDefault(const QString& id);

signals:
    void changed();

private:
    Command* find(const QString& id);
    const Command* find(const QString& id) const;

    /// Whether a suppressor would take this sequence: a bare single key it
    /// claims. Ctrl+E is never a note, and a two-step sequence never starts as
    /// one either.
    bool isSuppressed(const QKeySequence& seq) const;
    /// What the command is bound to, parked value first, then its action.
    QList<QKeySequence> boundKeys(const Command& command) const;
    /// Point a command at `seq`, honouring an installed suppressor.
    void bind(Command& command, const QKeySequence& seq);
    /// Re-derive every action's shortcuts from its binding and the suppressor,
    /// parking the bindings that lose keys so they can still be reported.
    void applySuppression();

    /// Canonical US-position form used on disk and by QAction. A binding made
    /// while a Cyrillic layout is active is therefore the same binding the
    /// user would have made on an English layout.
    static QKeySequence canonicalSequence(const QKeySequence& seq);
    /// Translate a physical key press to its US-QWERTY key and trigger the
    /// matching QAction when Qt's logical-layout shortcut pass could not.
    bool triggerPhysicalShortcut(QObject* watched, QKeyEvent* event);
    bool eventFilter(QObject* watched, QEvent* event) override;

    QVector<Command> m_commands;
    /// Bindings held back from their actions, by command id. Non-empty only
    /// while a suppressor is installed and only for the commands it touches.
    QHash<QString, QList<QKeySequence>> m_parked;
    std::function<bool(int)> m_suppressed;
};
