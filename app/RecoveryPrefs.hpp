#pragma once

/// Settings for crash recovery, under `recovery/*`. Free functions over
/// QSettings, matching `ui::browserprefs` and `ui::aiprefs`.

namespace ui::recoveryprefs {

/// Whether the journal runs at all. On by default: the cost is one small file
/// write every couple of seconds, and the thing it protects against is losing
/// an afternoon's work.
bool enabled();
void setEnabled(bool on);

/// Whether the watchdog process is started alongside. Separately switchable
/// because it is the part that shows up in Activity Monitor, and because
/// turning it off costs only hang detection and the health log — never the
/// journal, which is what actually preserves the work.
bool watchdog();
void setWatchdog(bool on);

} // namespace ui::recoveryprefs
