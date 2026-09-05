#pragma once

#include <QHash>
#include <QFlags>
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
    /// Consequence classification for callers that need to decide whether a
    /// command may run automatically. Unknown is the safe migration default
    /// for commands registered through the legacy overload.
    enum class Risk {
        Unknown,
        Safe,
        Reversible,
        Destructive,
        ExternalSideEffect,
    };

    /// Collaboration boundary for semantic UI commands. Remote execution is
    /// command-based: it never synthesizes a key, mouse event or QWidget click.
    enum class RemoteScope {
        Unclassified,
        SharedDocument,
        LocalSession,
        PresenterView,
        ForbiddenRemote,
    };

    /// Assistant interaction modes in which a command may be offered. This is
    /// metadata only: the policy layer remains responsible for enforcing it.
    enum Mode {
        HelpMode = 0x1,
        TeachMode = 0x2,
        DoMode = 0x4,
        AllModes = HelpMode | TeachMode | DoMode,
    };
    Q_DECLARE_FLAGS(ModeMask, Mode)

    struct Metadata {
        QString description;
        QString helpId;
        Risk risk = Risk::Unknown;
        ModeMask modes = AllModes;
        RemoteScope remoteScope = RemoteScope::Unclassified;
    };

    struct Command {
        QString id;
        QString label;
        QString category;
        QKeySequence defaultSeq;
        QAction* action = nullptr;   // the live action this command drives
        Metadata metadata;
    };

    explicit ShortcutManager(QObject* parent = nullptr);

    /// Register `action` under `id`; applies the saved-or-default shortcut to it.
    void registerCommand(const QString& id, const QString& label,
                         const QString& category, const QKeySequence& def,
                         QAction* action);
    /// Metadata-aware registration for semantic command discovery. The legacy
    /// overload above remains the normal shortcut-only path.
    void registerCommand(const QString& id, const QString& label,
                         const QString& category, const QKeySequence& def,
                         QAction* action, Metadata metadata);

    const QVector<Command>& commands() const { return m_commands; }
    /// Exact lookup by stable id. The pointer remains valid until another
    /// command is registered.
    const Command* command(const QString& id) const;
    /// Case-insensitive AND search across id, label, category, description and
    /// help id. An empty query returns every command in registration order.
    QVector<Command> search(const QString& query) const;
    /// Trigger a semantic command only when it exists and its QAction is both
    /// enabled and visible. Returns whether it was triggered.
    bool invoke(const QString& id) const;
    /// Invoke only an explicitly shareable semantic command. LocalSession and
    /// ForbiddenRemote are always refused. PresenterView additionally requires
    /// an active opt-in subscription from the receiving participant.
    bool invokeRemote(const QString& id,
                      bool presenterViewSubscribed = false) const;
    /// Used by the UI self-test/release gate so a newly registered command
    /// cannot silently cross the collaboration boundary.
    bool checkRemoteMetadata(QString* error = nullptr) const;
    static RemoteScope remoteScopeForId(const QString& id);

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

Q_DECLARE_OPERATORS_FOR_FLAGS(ShortcutManager::ModeMask)
