#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace daw {

/// What became of one `daw_scan` invocation.
struct ScanProcessResult {
    bool started = false;     ///< false when the executable could not be run
    bool timedOut = false;
    bool crashed = false;     ///< killed by a signal, or a non-zero exit
    int exitCode = -1;
    std::string output;       ///< everything the child wrote to stdout
    /// Human-readable, and shown next to a blacklisted plugin. "crashed
    /// (signal 11)" tells a user far more than a bare failure.
    std::string failureReason;

    bool succeeded() const noexcept {
        return started && !timedOut && !crashed && exitCode == 0;
    }
};

/// The descriptor a detached child inherits the parent-death pipe on. Fixed by
/// convention so the child needs no argument for it. 3 is the first free
/// descriptor after stdin/stdout/stderr.
inline constexpr int kParentPipeFd = 3;

/// Runs the scanner helper and collects its stdout, with a deadline.
///
/// Deliberately not QProcess: `daw_controller` is framework-agnostic by charter
/// and the Qt dependency stops at `app/`. This is `posix_spawn` plus a polled
/// read on POSIX and `CreateProcess` plus a reader thread on Windows.
class ScanProcess {
public:
    /// Blocking; the caller is the scan worker thread, never the audio thread.
    /// A child that outlives `timeout` is killed and reported as timed out —
    /// a hung plugin must not hold the scan up forever.
    static ScanProcessResult run(const std::string& executable,
                                 const std::vector<std::string>& arguments,
                                 std::chrono::milliseconds timeout);

    /// Start a process and do not wait for it. Returns its pid, or 0 on
    /// failure. Used for the crash watchdog, which has to outlive the call that
    /// launched it.
    ///
    /// On POSIX the returned `parentEndFd` is a pipe write end the caller must
    /// hold open and never write to: the child inherits the read end, so when
    /// this process dies for any reason at all — signal, kill -9, panic — the
    /// pipe closes and the child learns of it immediately, with no polling.
    /// Windows has no equivalent; there the child watches the process handle
    /// instead and `parentEndFd` comes back -1.
    static std::int64_t spawnDetached(const std::string& executable,
                                      const std::vector<std::string>& arguments,
                                      int* parentEndFd);
};

} // namespace daw
