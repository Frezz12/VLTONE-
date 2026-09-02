#pragma once

/// Where recovery sessions live, and the prompt the user meets after a crash.
///
/// The Qt-facing half of crash recovery; the journal itself is Qt-free and
/// lives in `controller/recovery`.

#include "recovery/SessionFile.hpp"

#include <QString>

class QWidget;
class QMessageBox;

namespace daw { class EngineController; }

namespace ui::recovery {

/// `<app data>/recovery`. Overridable with `DAW_RECOVERY_ROOT` so headless runs
/// and the crash test work in a temporary directory instead of the user's own.
QString rootDir();

/// Where `daw_guard` lives: a sibling of the running executable, the same
/// convention `daw_scan` uses. Empty when it is not there — a missing watchdog
/// costs statistics and hang detection, never the journal, so the program
/// carries on without it.
QString guardPath();

/// What the user chose in the recovery prompt.
struct Choice {
    bool restored = false;
    /// Where the recovered project belongs, empty if it was never saved. The
    /// caller adopts it as the project path so Ctrl+S goes where expected.
    QString projectPath;
};

/// Load one session's journal into the controller, replacing the document.
/// Shared by the prompt and by the headless recovery check, so the two cannot
/// drift apart. `error` is filled in on failure.
bool applySession(const daw::recovery::SessionInfo& session,
                  daw::EngineController& controller, QString* error);

/// Build the recovery prompt for one session, with Restore as the default
/// button. Shared by the live prompt and the screenshot hook so what gets
/// reviewed is what users actually see. Ownership passes to the caller.
QMessageBox* buildRecoveryPrompt(QWidget* parent,
                                 const daw::recovery::SessionInfo& session);

/// Offer every leftover session, newest first, and apply the one the user
/// accepts. Sessions the user declines are deleted unless they contain a cloud
/// recording sidecar; that sidecar and its WAV are preserved until the future
/// upload/commit recovery workflow owns their explicit cleanup. Sessions left
/// unanswered (the dialog was dismissed) are kept for next time.
Choice offerRecovery(QWidget* parent, daw::EngineController& controller);

/// A one-line description of how a session ended, for the prompt.
QString describeSession(const daw::recovery::SessionInfo& info);

} // namespace ui::recovery
