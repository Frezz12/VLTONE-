#pragma once

/// Layer 2 of crash recovery: what the program can say on its way out.
///
/// Be clear about what this is NOT. It does not save the project. Inside a
/// signal handler the heap may already be corrupt, and `malloc` — and therefore
/// serializing anything at all — is not async-signal-safe. A handler that tried
/// to "save normally" would crash a second time and take the recovery journal's
/// last write with it. The data is the journal's job, written continuously
/// ahead of time.
///
/// What this buys is the *reason*, which nothing else can supply: the signal,
/// the stack, and the plugin that was on it. In a DAW the answer is usually
/// third-party code, and "Serum crashed" is worth more to a user than any
/// number of stack frames.
///
/// The handler only ever `write(2)`s a pre-formatted buffer into a descriptor
/// opened at install time, then restores the default disposition and re-raises,
/// so the operating system still produces its own crash report and the exit
/// status honestly reflects the signal.

#include <string>

namespace daw::crash {

/// Install handlers for the fatal signals, writing to `markerPath`.
///
/// The file is created and opened now, while allocating is still safe. Does
/// nothing if `DAW_NO_CRASH_HANDLER` is set in the environment — a handler that
/// swallows faults makes a debugger useless.
///
/// Returns false when the marker could not be opened; the caller carries on
/// regardless, since losing the reason is much cheaper than losing the program.
bool install(const std::string& markerPath);

/// Restore the default dispositions. Mainly so tests can install, provoke, and
/// leave the process in a normal state.
void uninstall();

/// Name the third-party code currently on the stack, or nullptr on the way out.
/// Called around plugin loads and plugin calls; the handler reads it and puts
/// it in the marker.
///
/// The name is copied into a fixed static buffer rather than kept as a pointer:
/// a plugin's own std::string can be freed by the very fault being reported.
void setPluginInFlight(const std::string& name);
void clearPluginInFlight();

/// The most recently loaded plugin, whether or not one is in flight right now.
/// Reported in the health log, where "it died four seconds after loading
/// Serum" is often the whole diagnosis.
std::string lastPluginName();

/// What the last crash marker said, as one human-readable line — "crashed in
/// Serum (SIGSEGV)". Empty when the file is missing or unreadable. Called at
/// startup by the recovery prompt, where allocating is fine again.
std::string readMarker(const std::string& markerPath);

} // namespace daw::crash
