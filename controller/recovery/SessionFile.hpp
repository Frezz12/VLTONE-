#pragma once

/// The on-disk protocol between a running DAW and the `daw_guard` watchdog.
///
/// Header-only and deliberately dependency-light: the guard is a tiny separate
/// executable that links nothing but nlohmann/json, so it cannot see
/// `daw_controller`. Everything here is therefore plain std + json, and errors
/// are reported as `bool` rather than `audio::Result`, which lives in a library
/// the guard does not have.
///
/// The layout, under `<app data>/recovery/`:
///
///     <pid>-<startedUnixMs>/
///         session.json    what the guard reads; heartbeat and statistics
///         project.json    the journal — the document as of a few seconds ago
///
/// A clean shutdown deletes the whole session directory. **A directory that is
/// still there is the entire crash signal** — no flag to get wrong, and it
/// works even if the guard never ran and no signal handler ever fired.

#include <nlohmann/json.hpp>
#include "platform/PathUtils.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace daw::recovery {

inline constexpr const char* kSessionFile = "session.json";
inline constexpr const char* kJournalFile = "project.json";
/// Written by the crash handler on its way out — see crash/CrashHandler.hpp.
/// Parsed here rather than there so the watchdog, which cannot link
/// daw_controller, reads it exactly the way the DAW does.
inline constexpr const char* kCrashFile = "crash.txt";
inline constexpr const char* kSessionFormat = "daw-session";
inline constexpr int kSessionVersion = 1;

/// How the program was doing — the "статистика" half of the watchdog. Sampled
/// by the DAW, copied into session.json, and logged by the guard so a crash
/// report can say what the program was doing in the seconds before it died.
struct HealthStats {
    double processCpu = 0.0;          ///< percent; may exceed 100 on many cores
    double systemCpu = 0.0;           ///< 0…100 percent
    double dspLoad = 0.0;            ///< 0…1, the audio callback's duty cycle
    double dspLoadPeak = 0.0;
    std::uint64_t xruns = 0;
    std::uint64_t residentBytes = 0;
    double sampleRate = 0.0;
    int bufferFrames = 0;
    int trackCount = 0;
    int clipCount = 0;
    bool playing = false;
    bool recording = false;
    /// The last plugin the program loaded. Blank until one is. Third-party code
    /// is the usual cause of a DAW crash, so this is the single most useful
    /// field in the file.
    std::string lastPlugin;
};

/// How a session ended, as judged from outside. Written by the guard.
enum class Outcome {
    Running,   ///< no verdict yet
    Crashed,   ///< the process vanished without deleting its session directory
    Hung,      ///< alive, but the heartbeat stopped advancing
};

inline const char* toString(Outcome outcome) {
    switch (outcome) {
        case Outcome::Crashed: return "crashed";
        case Outcome::Hung: return "hung";
        case Outcome::Running: break;
    }
    return "running";
}

inline Outcome outcomeFromString(const std::string& text) {
    if (text == "crashed") return Outcome::Crashed;
    if (text == "hung") return Outcome::Hung;
    return Outcome::Running;
}

struct SessionInfo {
    std::string directory;      ///< absolute path to the session directory
    std::int64_t pid = 0;
    std::string appVersion;
    std::int64_t startedUnixMs = 0;
    /// Where the project lives, or empty when it has never been saved. This is
    /// what lets recovery put the work back where the user expects it.
    std::string projectPath;
    std::string projectName;
    /// Monotonically increasing. A frozen counter on a live process is a hang.
    std::uint64_t heartbeat = 0;
    std::int64_t heartbeatUnixMs = 0;
    std::int64_t journalUnixMs = 0;   ///< when project.json was last written
    HealthStats stats;
    Outcome outcome = Outcome::Running;
    /// Filled in by the crash handler when it manages to run: the signal, and
    /// the plugin that was on the stack. Empty when the process was killed
    /// outright or simply hung.
    std::string crashReason;
};

inline std::int64_t nowUnixMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}

inline std::int64_t currentProcessId() {
#if defined(_WIN32)
    return static_cast<std::int64_t>(::_getpid());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

/// Whether `pid` names a live process. Used to tell "the DAW is still running,
/// leave its session alone" from "this session is a leftover".
inline bool isProcessAlive(std::int64_t pid) {
    if (pid <= 0) return false;
#if defined(_WIN32)
    HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  static_cast<DWORD>(pid));
    if (!handle) return false;
    DWORD code = 0;
    const bool alive =
        ::GetExitCodeProcess(handle, &code) && code == STILL_ACTIVE;
    ::CloseHandle(handle);
    return alive;
#else
    // Signal 0 performs the permission and existence checks without delivering
    // anything. EPERM means the process exists but belongs to someone else.
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno == EPERM;
#endif
}

inline nlohmann::json statsToJson(const HealthStats& s) {
    return nlohmann::json{
        {"processCpu", s.processCpu},
        {"systemCpu", s.systemCpu},
        {"dspLoad", s.dspLoad},
        {"dspLoadPeak", s.dspLoadPeak},
        {"xruns", s.xruns},
        {"residentBytes", s.residentBytes},
        {"sampleRate", s.sampleRate},
        {"bufferFrames", s.bufferFrames},
        {"trackCount", s.trackCount},
        {"clipCount", s.clipCount},
        {"playing", s.playing},
        {"recording", s.recording},
        {"lastPlugin", s.lastPlugin},
    };
}

inline HealthStats statsFromJson(const nlohmann::json& j) {
    HealthStats s;
    if (!j.is_object()) return s;
    s.processCpu = j.value("processCpu", 0.0);
    s.systemCpu = j.value("systemCpu", 0.0);
    s.dspLoad = j.value("dspLoad", 0.0);
    s.dspLoadPeak = j.value("dspLoadPeak", 0.0);
    s.xruns = j.value("xruns", std::uint64_t{0});
    s.residentBytes = j.value("residentBytes", std::uint64_t{0});
    s.sampleRate = j.value("sampleRate", 0.0);
    s.bufferFrames = j.value("bufferFrames", 0);
    s.trackCount = j.value("trackCount", 0);
    s.clipCount = j.value("clipCount", 0);
    s.playing = j.value("playing", false);
    s.recording = j.value("recording", false);
    s.lastPlugin = j.value("lastPlugin", std::string());
    return s;
}

inline nlohmann::json sessionToJson(const SessionInfo& info) {
    return nlohmann::json{
        {"format", kSessionFormat},
        {"version", kSessionVersion},
        {"pid", info.pid},
        {"app", info.appVersion},
        {"startedUnixMs", info.startedUnixMs},
        {"projectPath", info.projectPath},
        {"projectName", info.projectName},
        {"heartbeat", info.heartbeat},
        {"heartbeatUnixMs", info.heartbeatUnixMs},
        {"journalUnixMs", info.journalUnixMs},
        {"stats", statsToJson(info.stats)},
        {"outcome", toString(info.outcome)},
        {"crashReason", info.crashReason},
    };
}

/// Replace a small JSON file without ever leaving a partial one in its place.
/// The guard polls session.json while the DAW rewrites it every second, so a
/// torn read is a real possibility rather than a theoretical one.
inline bool writeJsonAtomically(const nlohmann::json& root,
                                const std::filesystem::path& file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path temporary = file;
    temporary += ".tmp-" + std::to_string(currentProcessId());
    {
        std::ofstream os(temporary, std::ios::binary | std::ios::trunc);
        if (!os) return false;
        os << root.dump(2);
        os.flush();
        if (!os.good()) {
            os.close();
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, file, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(file, ignored);
        ec.clear();
        fs::rename(temporary, file, ec);
    }
    if (ec) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

inline bool writeSession(const SessionInfo& info) {
    return writeJsonAtomically(
        sessionToJson(info),
        platform::pathFromUtf8(info.directory) / kSessionFile);
}

inline bool readSession(const std::string& directory, SessionInfo& out) {
    namespace fs = std::filesystem;
    std::ifstream is(platform::pathFromUtf8(directory) / kSessionFile);
    if (!is) return false;
    nlohmann::json root;
    try {
        is >> root;
    } catch (const std::exception&) {
        // A torn read is expected occasionally; the caller simply tries again
        // on its next poll rather than treating it as a failed session.
        return false;
    }
    if (!root.is_object() || root.value("format", "") != kSessionFormat)
        return false;

    out = SessionInfo{};
    out.directory = directory;
    out.pid = root.value("pid", std::int64_t{0});
    out.appVersion = root.value("app", std::string());
    out.startedUnixMs = root.value("startedUnixMs", std::int64_t{0});
    out.projectPath = root.value("projectPath", std::string());
    out.projectName = root.value("projectName", std::string());
    out.heartbeat = root.value("heartbeat", std::uint64_t{0});
    out.heartbeatUnixMs = root.value("heartbeatUnixMs", std::int64_t{0});
    out.journalUnixMs = root.value("journalUnixMs", std::int64_t{0});
    if (root.contains("stats")) out.stats = statsFromJson(root.at("stats"));
    out.outcome = outcomeFromString(root.value("outcome", std::string()));
    out.crashReason = root.value("crashReason", std::string());
    return true;
}

/// Sessions under `root` that are not the caller's own and whose process is
/// gone — every one of them is unrecovered work.
///
/// A session whose process is still alive is a second DAW running right now and
/// is left strictly alone.
inline std::vector<SessionInfo> staleSessions(const std::string& root,
                                              const std::string& ownDirectory = {}) {
    namespace fs = std::filesystem;
    std::vector<SessionInfo> out;
    std::error_code ec;
    const fs::path nativeRoot = platform::pathFromUtf8(root);
    if (!fs::is_directory(nativeRoot, ec)) return out;
    for (const auto& entry : fs::directory_iterator(nativeRoot, ec)) {
        if (ec) break;
        if (!entry.is_directory()) continue;
        const std::string directory = platform::pathToUtf8(entry.path());
        if (!ownDirectory.empty() &&
            fs::equivalent(entry.path(), platform::pathFromUtf8(ownDirectory), ec) &&
            !ec) {
            continue;
        }
        ec.clear();
        SessionInfo info;
        if (!readSession(directory, info)) continue;
        if (isProcessAlive(info.pid)) continue;
        if (!fs::is_regular_file(entry.path() / kJournalFile, ec) || ec) {
            // A session that died before its first journal write holds nothing
            // worth offering. Clean it up rather than showing an empty prompt.
            std::error_code removeError;
            fs::remove_all(entry.path(), removeError);
            ec.clear();
            continue;
        }
        out.push_back(std::move(info));
    }
    return out;
}

/// One human-readable line from a crash marker — "crashed in Serum (SIGSEGV)".
/// Empty when there is no marker, which is the normal case: a program killed
/// outright, or one that simply froze, never gets to write one.
inline std::string parseCrashMarker(const std::string& filePath) {
    std::ifstream is(platform::pathFromUtf8(filePath));
    if (!is) return {};

    std::string signalText;
    std::string pluginText;
    std::string line;
    while (std::getline(is, line)) {
        if (line.rfind("signal=", 0) == 0) {
            signalText = line.substr(7);
            const std::size_t space = signalText.find(' ');
            if (space != std::string::npos) signalText.resize(space);
        } else if (line.rfind("plugin=", 0) == 0) {
            pluginText = line.substr(7);
        }
        if (!signalText.empty() && !pluginText.empty()) break;
    }
    if (signalText.empty()) return {};
    if (!pluginText.empty())
        return "crashed in " + pluginText + " (" + signalText + ")";
    return "crashed (" + signalText + ")";
}

inline void discardSession(const std::string& directory) {
    std::error_code ec;
    std::filesystem::remove_all(platform::pathFromUtf8(directory), ec);
}

} // namespace daw::recovery
