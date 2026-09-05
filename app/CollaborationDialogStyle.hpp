#pragma once

#include <QMargins>
#include <QString>

class QLabel;
class QWidget;

/// Shared construction and styling vocabulary for the collaboration dialogs.
///
/// CloudProjectInviteDialog established these conventions; CloudProjectsDialog
/// and JoinSessionDialog have to match them, and three copies of a secret-wipe
/// is two too many. Keeping them here makes the family consistent by
/// construction rather than by remembering to copy the last one.
// Named `dialog` rather than `ui` on purpose: these live inside namespace
// collab, where an inner `ui` would shadow the global `ui` namespace that
// holds IconButton, LevelMeter and the rest of Controls.hpp, and every call
// to those would silently fail to resolve.
namespace collab::dialog {

inline constexpr int kMinimumWidth = 460;
inline constexpr QMargins kMargins{24, 22, 24, 22};
inline constexpr int kSpacing = 14;

/// Server-sourced text is bounded before it reaches a widget. A server that
/// starts returning essays must not be able to resize a modal.
inline constexpr int kMaximumSafeMessageCharacters = 240;

/// The house dialog heading: bold, three points above the body text.
QLabel* titleLabel(const QString& text, QWidget* parent);

/// Trim, cap, and fall back when the result is empty. Never returns a string
/// longer than kMaximumSafeMessageCharacters.
QString boundedSafeMessage(const QString& value, const QString& fallback);

/// Translation contexts in this family are pinned with Q_DECLARE_TR_FUNCTIONS
/// on the feature's non-QObject state machine, and *every* string in that
/// feature — including the QDialog's own — goes through it.
///
/// This is not stylistic. lupdate emits an unqualified context name for classes
/// in a file that also uses Q_DECLARE_TR_FUNCTIONS, while Qt's tr() inside a
/// namespaced QObject looks up the namespaced name from the meta-object. The
/// two disagree, the lookup misses, and the dialog silently renders in English
/// in a translated build — with no compiler or runtime complaint. Routing
/// everything through one explicitly named context makes both tools agree.

/// Overwrite the buffer before releasing it. Best-effort — QString is
/// copy-on-write and a copy taken earlier is not reachable from here — which
/// is exactly why secrets are never copied out of the dialogs.
void wipe(QString& value);

/// Lowercase canonical form of a UUID, or empty when `value` is not one.
/// Used to refuse a malformed project id before it reaches a request.
QString canonicalUuid(const QString& value);

/// QListWidget, QTreeWidget, QGroupBox, QProgressBar and QScrollArea are not
/// covered by the global stylesheet (Theme.cpp), so a dialog containing them
/// falls back to raw Fusion and reads as a different application. Apply this
/// from the dialog's applyTheme(), wired to ThemeManager::changed as usual.
///
/// Object names the sheet styles, for dialogs that want them:
///   #CollabSecondary  — secondary/detail text
///   #CollabWarning    — a non-blocking problem
///   #CollabError      — a blocking problem
///   #CollabCode       — a monospaced invite code, shown large
QString styleSheet();

} // namespace collab::dialog
