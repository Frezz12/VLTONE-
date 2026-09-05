#include "ShortcutManager.hpp"
#include "KeyboardLayout.hpp"

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QTextEdit>
#include <QWidget>

#include <algorithm>
#include <array>

namespace {
QString settingsKey(const QString& id) { return "shortcuts/" + id; }

ShortcutManager::Metadata inferredMetadata(const QString& id,
                                           const QString& label) {
    ShortcutManager::Metadata metadata;
    metadata.description = label;
    metadata.helpId = id;
    const QString lower = id.toLower();
    if (lower == "app.quit" || lower == "file.new" ||
        lower == "file.open" || lower == "edit.undo" ||
        lower == "edit.redo" || lower.contains("delete") ||
        lower.contains("remove") || lower.contains("clear") ||
        lower.contains("cut")) {
        metadata.risk = ShortcutManager::Risk::Destructive;
    } else if (lower.startsWith("file.save") ||
               lower.startsWith("file.export") ||
               lower.startsWith("file.import")) {
        metadata.risk = ShortcutManager::Risk::ExternalSideEffect;
    } else if (lower.startsWith("view.") || lower.startsWith("window.") ||
               lower.startsWith("help.") ||
               lower.startsWith("transport.")) {
        metadata.risk = ShortcutManager::Risk::Safe;
    } else {
        metadata.risk = ShortcutManager::Risk::Reversible;
    }
    metadata.remoteScope = ShortcutManager::remoteScopeForId(id);
    return metadata;
}

struct PhysicalKey {
    quint32 native;
    int qt;
};

template <std::size_t N>
int fromTable(quint32 native, const std::array<PhysicalKey, N>& table) {
    for (const auto& item : table)
        if (item.native == native) return item.qt;
    return 0;
}

// macOS virtual key codes are hardware positions (Carbon/NSEvent keyCode), so
// they stay the same when the input source changes to Russian, German, etc.
constexpr std::array kMacKeys{
    PhysicalKey{0x00, Qt::Key_A}, PhysicalKey{0x01, Qt::Key_S},
    PhysicalKey{0x02, Qt::Key_D}, PhysicalKey{0x03, Qt::Key_F},
    PhysicalKey{0x04, Qt::Key_H}, PhysicalKey{0x05, Qt::Key_G},
    PhysicalKey{0x06, Qt::Key_Z}, PhysicalKey{0x07, Qt::Key_X},
    PhysicalKey{0x08, Qt::Key_C}, PhysicalKey{0x09, Qt::Key_V},
    PhysicalKey{0x0b, Qt::Key_B}, PhysicalKey{0x0c, Qt::Key_Q},
    PhysicalKey{0x0d, Qt::Key_W}, PhysicalKey{0x0e, Qt::Key_E},
    PhysicalKey{0x0f, Qt::Key_R}, PhysicalKey{0x10, Qt::Key_Y},
    PhysicalKey{0x11, Qt::Key_T}, PhysicalKey{0x12, Qt::Key_1},
    PhysicalKey{0x13, Qt::Key_2}, PhysicalKey{0x14, Qt::Key_3},
    PhysicalKey{0x15, Qt::Key_4}, PhysicalKey{0x16, Qt::Key_6},
    PhysicalKey{0x17, Qt::Key_5}, PhysicalKey{0x18, Qt::Key_Equal},
    PhysicalKey{0x19, Qt::Key_9}, PhysicalKey{0x1a, Qt::Key_7},
    PhysicalKey{0x1b, Qt::Key_Minus}, PhysicalKey{0x1c, Qt::Key_8},
    PhysicalKey{0x1d, Qt::Key_0}, PhysicalKey{0x1e, Qt::Key_BracketRight},
    PhysicalKey{0x1f, Qt::Key_O}, PhysicalKey{0x20, Qt::Key_U},
    PhysicalKey{0x21, Qt::Key_BracketLeft}, PhysicalKey{0x22, Qt::Key_I},
    PhysicalKey{0x23, Qt::Key_P}, PhysicalKey{0x25, Qt::Key_L},
    PhysicalKey{0x26, Qt::Key_J}, PhysicalKey{0x27, Qt::Key_Apostrophe},
    PhysicalKey{0x28, Qt::Key_K}, PhysicalKey{0x29, Qt::Key_Semicolon},
    PhysicalKey{0x2a, Qt::Key_Backslash}, PhysicalKey{0x2b, Qt::Key_Comma},
    PhysicalKey{0x2c, Qt::Key_Slash}, PhysicalKey{0x2d, Qt::Key_N},
    PhysicalKey{0x2e, Qt::Key_M}, PhysicalKey{0x2f, Qt::Key_Period},
    PhysicalKey{0x32, Qt::Key_QuoteLeft},
};

// Windows exposes the Set-1 scan position in nativeScanCode. Linux evdev uses
// the same positions; X11 adds eight, handled below.
constexpr std::array kPcKeys{
    PhysicalKey{0x02, Qt::Key_1}, PhysicalKey{0x03, Qt::Key_2},
    PhysicalKey{0x04, Qt::Key_3}, PhysicalKey{0x05, Qt::Key_4},
    PhysicalKey{0x06, Qt::Key_5}, PhysicalKey{0x07, Qt::Key_6},
    PhysicalKey{0x08, Qt::Key_7}, PhysicalKey{0x09, Qt::Key_8},
    PhysicalKey{0x0a, Qt::Key_9}, PhysicalKey{0x0b, Qt::Key_0},
    PhysicalKey{0x0c, Qt::Key_Minus}, PhysicalKey{0x0d, Qt::Key_Equal},
    PhysicalKey{0x10, Qt::Key_Q}, PhysicalKey{0x11, Qt::Key_W},
    PhysicalKey{0x12, Qt::Key_E}, PhysicalKey{0x13, Qt::Key_R},
    PhysicalKey{0x14, Qt::Key_T}, PhysicalKey{0x15, Qt::Key_Y},
    PhysicalKey{0x16, Qt::Key_U}, PhysicalKey{0x17, Qt::Key_I},
    PhysicalKey{0x18, Qt::Key_O}, PhysicalKey{0x19, Qt::Key_P},
    PhysicalKey{0x1a, Qt::Key_BracketLeft}, PhysicalKey{0x1b, Qt::Key_BracketRight},
    PhysicalKey{0x1e, Qt::Key_A}, PhysicalKey{0x1f, Qt::Key_S},
    PhysicalKey{0x20, Qt::Key_D}, PhysicalKey{0x21, Qt::Key_F},
    PhysicalKey{0x22, Qt::Key_G}, PhysicalKey{0x23, Qt::Key_H},
    PhysicalKey{0x24, Qt::Key_J}, PhysicalKey{0x25, Qt::Key_K},
    PhysicalKey{0x26, Qt::Key_L}, PhysicalKey{0x27, Qt::Key_Semicolon},
    PhysicalKey{0x28, Qt::Key_Apostrophe}, PhysicalKey{0x29, Qt::Key_QuoteLeft},
    PhysicalKey{0x2b, Qt::Key_Backslash}, PhysicalKey{0x2c, Qt::Key_Z},
    PhysicalKey{0x2d, Qt::Key_X}, PhysicalKey{0x2e, Qt::Key_C},
    PhysicalKey{0x2f, Qt::Key_V}, PhysicalKey{0x30, Qt::Key_B},
    PhysicalKey{0x31, Qt::Key_N}, PhysicalKey{0x32, Qt::Key_M},
    PhysicalKey{0x33, Qt::Key_Comma}, PhysicalKey{0x34, Qt::Key_Period},
    PhysicalKey{0x35, Qt::Key_Slash},
};

int latinKeyForCharacter(QChar character) {
    const ushort u = character.toLower().unicode();
    switch (u) {
        // Russian ЙЦУКЕН, mapped by physical US-QWERTY position.
        case 0x0439: return Qt::Key_Q; case 0x0446: return Qt::Key_W;
        case 0x0443: return Qt::Key_E; case 0x043a: return Qt::Key_R;
        case 0x0435: return Qt::Key_T; case 0x043d: return Qt::Key_Y;
        case 0x0433: return Qt::Key_U; case 0x0448: return Qt::Key_I;
        case 0x0449: return Qt::Key_O; case 0x0437: return Qt::Key_P;
        case 0x0445: return Qt::Key_BracketLeft;
        case 0x044a: return Qt::Key_BracketRight;
        case 0x0444: return Qt::Key_A; case 0x044b: return Qt::Key_S;
        case 0x0432: return Qt::Key_D; case 0x0430: return Qt::Key_F;
        case 0x043f: return Qt::Key_G; case 0x0440: return Qt::Key_H;
        case 0x043e: return Qt::Key_J; case 0x043b: return Qt::Key_K;
        case 0x0434: return Qt::Key_L; case 0x0436: return Qt::Key_Semicolon;
        case 0x044d: return Qt::Key_Apostrophe;
        case 0x044f: return Qt::Key_Z; case 0x0447: return Qt::Key_X;
        case 0x0441: return Qt::Key_C; case 0x043c: return Qt::Key_V;
        case 0x0438: return Qt::Key_B; case 0x0442: return Qt::Key_N;
        case 0x044c: return Qt::Key_M; case 0x0431: return Qt::Key_Comma;
        case 0x044e: return Qt::Key_Period; case 0x0451: return Qt::Key_QuoteLeft;
        default: return 0;
    }
}

int physicalUsKeyImpl(const QKeyEvent* event) {
#if defined(Q_OS_MACOS)
    if (const int key = fromTable(event->nativeVirtualKey(), kMacKeys)) return key;
#elif defined(Q_OS_WIN)
    if (const int key = fromTable(event->nativeScanCode() & 0xffu, kPcKeys)) return key;
#elif defined(Q_OS_LINUX)
    quint32 scan = event->nativeScanCode();
    if (QApplication::platformName() == QStringLiteral("xcb") && scan >= 8)
        scan -= 8;
    if (const int key = fromTable(scan, kPcKeys)) return key;
#endif
    // Remote desktops and synthetic events do not always carry native codes.
    // The Cyrillic fallback still covers the common layout explicitly.
    if (!event->text().isEmpty())
        if (const int key = latinKeyForCharacter(event->text().front())) return key;
    return 0;
}

Qt::KeyboardModifiers shortcutModifiers(Qt::KeyboardModifiers modifiers) {
    return modifiers & (Qt::ShiftModifier | Qt::ControlModifier |
                        Qt::AltModifier | Qt::MetaModifier);
}

bool belongsTo(QObject* object, QWidget* window) {
    for (QObject* p = object; p; p = p->parent()) {
        if (p == window) return true;
        if (auto* widget = qobject_cast<QWidget*>(p); widget && widget->window() == window)
            return true;
    }
    return false;
}

bool isTextEntry(QWidget* widget) {
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (current->property("dawWebInput").toBool()) return true;
        if (qobject_cast<QLineEdit*>(current) ||
            qobject_cast<QTextEdit*>(current) ||
            qobject_cast<QPlainTextEdit*>(current) ||
            qobject_cast<QAbstractSpinBox*>(current) ||
            qobject_cast<QKeySequenceEdit*>(current) ||
            (qobject_cast<QComboBox*>(current) &&
             qobject_cast<QComboBox*>(current)->isEditable())) {
            return true;
        }
    }
    return false;
}
} // namespace

int ui::physicalUsKey(const QKeyEvent* event) {
    return physicalUsKeyImpl(event);
}

ShortcutManager::ShortcutManager(QObject* parent) : QObject(parent) {
    // One application-level filter covers the main arrangement, detached
    // mixer/editor windows and actions created later (for example Piano Roll).
    if (qApp) qApp->installEventFilter(this);
}

ShortcutManager::RemoteScope ShortcutManager::remoteScopeForId(
    const QString& id) {
    const QString lower = id.toLower();
    if (lower.startsWith(QStringLiteral("file.")) ||
        lower.startsWith(QStringLiteral("app.")) ||
        lower.startsWith(QStringLiteral("browser.")) ||
        lower.startsWith(QStringLiteral("timeline.")) ||
        lower == QLatin1String("track.findplugin") ||
        lower == QLatin1String("view.togglebrowser") ||
        lower == QLatin1String("view.toggleweb") ||
        lower == QLatin1String("view.toggleai") ||
        lower == QLatin1String("edit.copyclips") ||
        lower == QLatin1String("edit.cutclips") ||
        lower == QLatin1String("edit.pasteclips")) {
        return RemoteScope::ForbiddenRemote;
    }
    if (lower.startsWith(QStringLiteral("transport.")) ||
        lower.startsWith(QStringLiteral("tool.")) ||
        lower.startsWith(QStringLiteral("edit.grid.")) ||
        lower == QLatin1String("edit.snapon") ||
        lower == QLatin1String("edit.snapoff")) {
        return RemoteScope::LocalSession;
    }
    if (lower.startsWith(QStringLiteral("view.")) ||
        lower.startsWith(QStringLiteral("editor.")) ||
        lower == QLatin1String("track.automation") ||
        lower == QLatin1String("edit.togglecomp")) {
        return RemoteScope::PresenterView;
    }
    if (lower.startsWith(QStringLiteral("edit.")) ||
        lower.startsWith(QStringLiteral("track."))) {
        return RemoteScope::SharedDocument;
    }
    return RemoteScope::Unclassified;
}

void ShortcutManager::registerCommand(const QString& id, const QString& label,
                                      const QString& category,
                                      const QKeySequence& def, QAction* action) {
    registerCommand(id, label, category, def, action,
                    inferredMetadata(id, label));
}

void ShortcutManager::registerCommand(const QString& id, const QString& label,
                                      const QString& category,
                                      const QKeySequence& def, QAction* action,
                                      Metadata metadata) {
    Command c;
    c.id = id;
    c.label = label;
    c.category = category;
    c.defaultSeq = canonicalSequence(def);
    c.action = action;
    c.metadata = std::move(metadata);
    if (action) action->setObjectName(id);

    // Saved override wins over the default; a saved empty string means the user
    // deliberately unbound the command.
    QSettings settings;
    const QString key = settingsKey(id);
    QKeySequence seq = c.defaultSeq;
    if (settings.contains(key)) {
        seq = canonicalSequence(QKeySequence::fromString(
            settings.value(key).toString(), QKeySequence::PortableText));
    }
    if (action) action->setShortcut(seq);

    m_commands.push_back(c);
    // A command registered while the typing keyboard is on must not arrive with
    // a live letter shortcut. Nothing does today — the menus are built once, at
    // startup — but the invariant is cheap to keep.
    if (m_suppressed) applySuppression();
}

ShortcutManager::Command* ShortcutManager::find(const QString& id) {
    for (auto& c : m_commands)
        if (c.id == id) return &c;
    return nullptr;
}

const ShortcutManager::Command* ShortcutManager::find(const QString& id) const {
    for (const auto& c : m_commands)
        if (c.id == id) return &c;
    return nullptr;
}

const ShortcutManager::Command* ShortcutManager::command(
    const QString& id) const {
    return find(id);
}

QVector<ShortcutManager::Command> ShortcutManager::search(
    const QString& query) const {
    const QStringList terms =
        query.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (terms.isEmpty()) return m_commands;

    QVector<Command> matches;
    for (const Command& candidate : m_commands) {
        const auto hasTerm = [&candidate](const QString& term) {
            return candidate.id.contains(term, Qt::CaseInsensitive) ||
                   candidate.label.contains(term, Qt::CaseInsensitive) ||
                   candidate.category.contains(term, Qt::CaseInsensitive) ||
                   candidate.metadata.description.contains(
                       term, Qt::CaseInsensitive) ||
                   candidate.metadata.helpId.contains(term,
                                                      Qt::CaseInsensitive);
        };
        if (std::all_of(terms.cbegin(), terms.cend(), hasTerm))
            matches.push_back(candidate);
    }
    return matches;
}

bool ShortcutManager::invoke(const QString& id) const {
    const Command* candidate = find(id);
    if (!candidate || !candidate->action ||
        !candidate->action->isEnabled() ||
        !candidate->action->isVisible()) {
        return false;
    }
    candidate->action->trigger();
    return true;
}

bool ShortcutManager::invokeRemote(const QString& id,
                                   bool presenterViewSubscribed) const {
    const Command* candidate = find(id);
    if (!candidate) return false;
    const RemoteScope scope = candidate->metadata.remoteScope;
    if (scope != RemoteScope::SharedDocument &&
        !(scope == RemoteScope::PresenterView &&
          presenterViewSubscribed)) {
        return false;
    }
    return invoke(id);
}

bool ShortcutManager::checkRemoteMetadata(QString* error) const {
    QSet<QString> ids;
    for (const Command& candidate : m_commands) {
        if (candidate.id.isEmpty() || ids.contains(candidate.id)) {
            if (error) *error = QStringLiteral("duplicate or empty command id");
            return false;
        }
        ids.insert(candidate.id);
        if (candidate.metadata.remoteScope == RemoteScope::Unclassified) {
            if (error)
                *error = QStringLiteral("unclassified remote command: %1")
                             .arg(candidate.id);
            return false;
        }
        if ((candidate.metadata.remoteScope == RemoteScope::ForbiddenRemote ||
             candidate.metadata.remoteScope == RemoteScope::LocalSession) &&
            invokeRemote(candidate.id, true)) {
            if (error)
                *error = QStringLiteral("forbidden remote command invoked: %1")
                             .arg(candidate.id);
            return false;
        }
    }
    return true;
}

bool ShortcutManager::isSuppressed(const QKeySequence& seq) const {
    return m_suppressed && seq.count() == 1 &&
           seq[0].keyboardModifiers() == Qt::NoModifier &&
           m_suppressed(seq[0].key());
}

QList<QKeySequence> ShortcutManager::boundKeys(const Command& command) const {
    const auto parked = m_parked.constFind(command.id);
    if (parked != m_parked.constEnd()) return *parked;
    return command.action ? command.action->shortcuts() : QList<QKeySequence>{};
}

void ShortcutManager::applySuppression() {
    for (Command& c : m_commands) {
        if (!c.action) continue;
        const QList<QKeySequence> bound = boundKeys(c);
        QList<QKeySequence> live;
        for (const QKeySequence& seq : bound) {
            if (!isSuppressed(seq)) live.push_back(seq);
        }
        // Only a binding that actually lost a key needs parking; anything else
        // is fully described by its own action.
        if (live.size() == bound.size()) {
            m_parked.remove(c.id);
        } else {
            m_parked.insert(c.id, bound);
        }
        c.action->setShortcuts(live);
    }
}

void ShortcutManager::setKeySuppressor(std::function<bool(int)> suppressed) {
    m_suppressed = std::move(suppressed);
    applySuppression();
    emit changed();
}

QKeySequence ShortcutManager::shortcut(const QString& id) const {
    const Command* c = find(id);
    if (!c) return {};
    const QList<QKeySequence> bound = boundKeys(*c);
    return bound.isEmpty() ? QKeySequence() : bound.first();
}

QKeySequence ShortcutManager::defaultShortcut(const QString& id) const {
    const Command* c = find(id);
    return c ? c->defaultSeq : QKeySequence();
}

QString ShortcutManager::label(const QString& id) const {
    const Command* c = find(id);
    return c ? c->label : QString();
}

QString ShortcutManager::conflict(const QString& id,
                                  const QKeySequence& seq) const {
    const QKeySequence canonical = canonicalSequence(seq);
    if (canonical.isEmpty()) return {};
    for (const auto& c : m_commands) {
        if (c.id == id || !c.action) continue;
        // Against the binding, not the action: a command whose key is parked
        // for the typing keyboard still owns that key, and handing it to a
        // second command would leave both of them on it once it comes back.
        if (boundKeys(c).contains(canonical)) return c.id;
    }
    return {};
}

void ShortcutManager::setShortcut(const QString& id, const QKeySequence& seq) {
    Command* c = find(id);
    if (!c) return;
    const QKeySequence canonical = canonicalSequence(seq);
    bind(*c, canonical);

    QSettings settings;
    settings.setValue(settingsKey(id),
                      canonical.toString(QKeySequence::PortableText));
    emit changed();
}

void ShortcutManager::resetToDefault(const QString& id) {
    Command* c = find(id);
    if (!c) return;
    bind(*c, c->defaultSeq);

    QSettings settings;
    settings.remove(settingsKey(id));
    emit changed();
}

void ShortcutManager::bind(Command& command, const QKeySequence& seq) {
    QList<QKeySequence> bound;
    if (!seq.isEmpty()) bound.push_back(seq);
    // Park it first so applySuppression treats this as the binding rather than
    // reading the stale one off the action; it drops the park again when the
    // suppressor takes nothing away.
    m_parked.insert(command.id, bound);
    applySuppression();
}

QKeySequence ShortcutManager::canonicalSequence(const QKeySequence& seq) {
    if (seq.isEmpty()) return {};
    std::array<int, 4> combined{};
    for (qsizetype i = 0; i < seq.count() && i < qsizetype(combined.size()); ++i) {
        const QKeyCombination source = seq[i];
        int key = source.key();
        // Qt represents non-Latin Unicode keys as 0x01000000 | codepoint.
        // This also makes shortcuts captured by QKeySequenceEdit under Russian
        // layout canonical instead of permanently layout-specific.
        const uint codepoint = uint(key) & 0x00ffffffu;
        if (codepoint <= 0xffffu) {
            if (const int latin = latinKeyForCharacter(QChar(ushort(codepoint))))
                key = latin;
        }
        combined[size_t(i)] =
            QKeyCombination(shortcutModifiers(source.keyboardModifiers()),
                            Qt::Key(key)).toCombined();
    }
    return QKeySequence(combined[0], combined[1], combined[2], combined[3]);
}

bool ShortcutManager::triggerPhysicalShortcut(QObject* watched,
                                               QKeyEvent* event) {
    const int physicalKey = ui::physicalUsKey(event);
    if (!physicalKey || physicalKey == event->key()) return false;

    QWidget* focus = QApplication::focusWidget();
    if (!focus) focus = qobject_cast<QWidget*>(watched);
    // A name, tempo or shortcut field owns all of its keystrokes. In
    // particular Ctrl+C/X/V must keep editing text instead of clips.
    if (isTextEntry(focus)) return false;
    if (QApplication::activePopupWidget()) return false;

    QWidget* active = QApplication::activeModalWidget();
    if (!active && focus) active = focus->window();
    if (!active) active = QApplication::activeWindow();
    if (!active) return false;

    const QKeyCombination wanted(shortcutModifiers(event->modifiers()),
                                 Qt::Key(physicalKey));
    QSet<QAction*> actions;
    for (QWidget* top : QApplication::topLevelWidgets()) {
        for (QAction* action : top->actions()) actions.insert(action);
        for (QAction* action : top->findChildren<QAction*>()) actions.insert(action);
    }
    for (QAction* action : qApp->findChildren<QAction*>()) actions.insert(action);

    QAction* match = nullptr;
    int bestPriority = -1;
    bool ambiguous = false;
    const bool modal = QApplication::activeModalWidget();
    for (QAction* action : actions) {
        if (!action || !action->isEnabled() || !action->isVisible()) continue;
        bool hasKey = false;
        for (const QKeySequence& seq : action->shortcuts()) {
            if (seq.count() == 1 && seq[0] == wanted) {
                hasKey = true;
                break;
            }
        }
        if (!hasKey) continue;

        const auto associated = action->associatedObjects();
        const bool inWindow = belongsTo(action, active) ||
            std::any_of(associated.begin(), associated.end(),
                        [active](QObject* object) { return belongsTo(object, active); });
        const bool atFocus = focus && std::any_of(
            associated.begin(), associated.end(), [focus](QObject* object) {
                auto* widget = qobject_cast<QWidget*>(object);
                return widget && widget == focus;
            });
        const bool containsFocus = focus && std::any_of(
            associated.begin(), associated.end(), [focus](QObject* object) {
                auto* widget = qobject_cast<QWidget*>(object);
                return widget && (widget == focus || widget->isAncestorOf(focus));
            });

        int priority = -1;
        switch (action->shortcutContext()) {
            case Qt::WidgetShortcut: priority = atFocus ? 3 : -1; break;
            case Qt::WidgetWithChildrenShortcut:
                priority = containsFocus ? 2 : -1;
                break;
            case Qt::WindowShortcut: priority = inWindow ? 1 : -1; break;
            case Qt::ApplicationShortcut:
                // Match Qt's modality rule: an application shortcut outside a
                // modal dialog must wait until the dialog closes.
                priority = (!modal || inWindow) ? 0 : -1;
                break;
        }
        if (priority < 0 || priority < bestPriority) continue;
        if (priority > bestPriority) {
            match = action;
            bestPriority = priority;
            ambiguous = false;
            continue;
        }
        if (match && match != action) {
            // Same ambiguity Qt reports for two native shortcuts: don't guess.
            ambiguous = true;
            continue;
        }
        if (!match) match = action;
    }
    if (!match || ambiguous) return false;
    event->accept();
    match->trigger();
    return true;
}

bool ShortcutManager::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        if (triggerPhysicalShortcut(watched, static_cast<QKeyEvent*>(event)))
            return true;
    }
    return QObject::eventFilter(watched, event);
}
