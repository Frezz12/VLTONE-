// daw_guard — the crash watchdog.
//
// A separate executable on purpose, but NOT for the reason people expect: it
// cannot save a crashed DAW's project. The document lives in the dead process's
// memory and is gone the moment it dies. Preserving the work is the journal's
// job, inside the DAW, writing continuously ahead of time.
//
// What this process can do — and what nothing inside the DAW can — is:
//
//   * notice a HANG. A frozen program runs no signal handler and no timer; only
//     something outside it can tell that its heartbeat stopped.
//   * survive the crash long enough to record a verdict, so the next launch
//     knows the session ended badly rather than guessing.
//   * keep a health log, so a crash report can say what the program was doing
//     in the seconds before it died — DSP load, xruns, the last plugin loaded.
//
// It deliberately contains nothing but file reads and a wait: no Qt, no audio,
// no plugin code. A watchdog that can itself crash is worse than none.
//
// Death is detected without polling. The DAW hands us the read end of a pipe it
// holds open forever; when it dies for any reason at all — signal, kill -9,
// kernel panic — the write end closes and we see EOF at once.

#include "recovery/SessionFile.hpp"
#include "platform/PathUtils.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace daw::recovery;

namespace {

/// Matches the DAW's one-second heartbeat with room for a slow disk or a
/// stop-the-world moment that is not a real hang. Overridable so the test does
/// not have to sit through the production value.
constexpr int kDefaultHangSeconds = 10;
constexpr int kPollMillis = 1000;
/// Health samples are appended forever otherwise; a day of use at one line a
/// second is a few megabytes, so the log is rotated well before that.
constexpr std::uintmax_t kMaxLogBytes = 2 * 1024 * 1024;

/// The descriptor spawnDetached dup2's the parent-death pipe onto.
constexpr int kParentPipeFd = 3;

void appendHealth(const fs::path& logFile, const SessionInfo& info) {
    std::error_code ec;
    if (fs::is_regular_file(logFile, ec) &&
        fs::file_size(logFile, ec) > kMaxLogBytes && !ec) {
        fs::path rotated = logFile;
        rotated += ".1";
        fs::rename(logFile, rotated, ec);
    }
    std::ofstream os(logFile, std::ios::app);
    if (!os) return;
    nlohmann::json line = statsToJson(info.stats);
    line["unixMs"] = info.heartbeatUnixMs;
    line["heartbeat"] = info.heartbeat;
    os << line.dump() << '\n';
}

/// Record how the session ended, without disturbing anything else the file
/// holds — it is re-read first so a verdict never overwrites a heartbeat or a
/// crash reason the dying process managed to write.
void recordOutcome(const std::string& directory, Outcome outcome) {
    SessionInfo info;
    if (!readSession(directory, info)) return;
    // The crash handler may have left a marker naming the signal and the plugin
    // that was on the stack. It could not write JSON — the heap was suspect —
    // so folding its line into the session is this process's job.
    std::string reason;
    if (outcome == Outcome::Crashed) {
        reason = parseCrashMarker(daw::platform::pathToUtf8(
            daw::platform::pathFromUtf8(directory) / kCrashFile));
    }
    if (info.outcome == outcome && info.crashReason == reason) return;
    info.outcome = outcome;
    if (!reason.empty()) info.crashReason = reason;
    writeSession(info);
}

/// True when the parent is gone. Blocks up to `millis` waiting for that, so the
/// caller's loop is driven by the parent's death rather than by a timer.
bool waitForParentDeath(int millis, std::int64_t pid) {
#if defined(_WIN32)
    HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!handle) return true;   // already gone
    const DWORD waited = ::WaitForSingleObject(handle, static_cast<DWORD>(millis));
    ::CloseHandle(handle);
    return waited == WAIT_OBJECT_0;
#else
    (void)pid;
    pollfd descriptor{};
    descriptor.fd = kParentPipeFd;
    // POLLIN, even though nothing is ever written: macOS does not report
    // POLLHUP at all unless some event was actually requested (verified — with
    // events = 0 the hangup goes unnoticed forever and the watchdog never
    // learns its parent died). Readability on a pipe with no writer IS the
    // end-of-file, so this is the portable way to ask the question.
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, millis);
    if (ready < 0) return false;                 // EINTR: try again
    if (ready == 0) return false;                // timed out, parent still there
    return (descriptor.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
#endif
}

int usage() {
    std::fprintf(stderr,
                 "usage: daw_guard --session <dir> [--pid <pid>] "
                 "[--hang-seconds <n>]\n");
    return 2;
}

int guardMain(const std::vector<std::string>& arguments) {
    std::string sessionDir;
    std::int64_t parentPid = 0;
    int hangSeconds = kDefaultHangSeconds;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == "--session" && i + 1 < arguments.size())
            sessionDir = arguments[++i];
        else if (arguments[i] == "--pid" && i + 1 < arguments.size())
            parentPid = std::atoll(arguments[++i].c_str());
        else if (arguments[i] == "--hang-seconds" &&
                 i + 1 < arguments.size())
            hangSeconds = std::atoi(arguments[++i].c_str());
        else
            return usage();
    }
    if (sessionDir.empty()) return usage();

    const fs::path nativeSessionDir =
        daw::platform::pathFromUtf8(sessionDir);
    const fs::path logFile = nativeSessionDir / "health.jsonl";
    std::uint64_t lastHeartbeat = 0;
    int secondsSinceHeartbeat = 0;
    bool reportedHang = false;

    for (;;) {
        const bool parentGone = waitForParentDeath(kPollMillis, parentPid);

        std::error_code ec;
        // The session directory disappearing IS the clean-shutdown signal: the
        // DAW deletes it on its way out. Nothing to judge, nothing to report.
        if (!fs::is_directory(nativeSessionDir, ec) || ec) return 0;

        SessionInfo info;
        const bool readable = readSession(sessionDir, info);

        if (parentGone) {
            // The pipe closed but the directory is still here — the DAW died
            // without tidying up. That is the whole verdict; recovery on the
            // next launch does the rest.
            if (readable) recordOutcome(sessionDir, Outcome::Crashed);
            return 0;
        }

        if (!readable) continue;   // a torn read; try again next second

        appendHealth(logFile, info);

        // A heartbeat that stops advancing on a process that is still alive is
        // a freeze — the one failure no in-process handler can ever catch.
        if (info.heartbeat != lastHeartbeat) {
            lastHeartbeat = info.heartbeat;
            secondsSinceHeartbeat = 0;
            if (reportedHang) {
                // It came back. Say so, rather than leaving a stale verdict
                // that would make the next launch cry crash over nothing.
                recordOutcome(sessionDir, Outcome::Running);
                reportedHang = false;
            }
        } else if (++secondsSinceHeartbeat >= hangSeconds && !reportedHang) {
            recordOutcome(sessionDir, Outcome::Hung);
            reportedHang = true;
        }
    }
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? std::size_t(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        arguments.push_back(daw::platform::pathToUtf8(
            std::filesystem::path(std::wstring(argv[i]))));
    }
    return guardMain(arguments);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc > 1 ? std::size_t(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    return guardMain(arguments);
}
#endif
