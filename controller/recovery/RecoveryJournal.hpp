#pragma once

/// Layer 1 of crash recovery: a continuous, cheap copy of the document on disk.
///
/// This is the only part of the system that actually preserves work. A watchdog
/// process cannot save a crashed program's project — the document lives in the
/// dead process's memory and is gone the moment it dies — so the data has to be
/// written out ahead of time, by the program itself, continuously.
///
/// "Cheap" is the whole design constraint. `ProjectSerializer::save` copies
/// every referenced audio file into the package, which is fine once when the
/// user presses Ctrl+S and ruinous every few seconds. The journal writes
/// `ProjectSerializer::saveDocument` with `MediaPaths::Absolute` instead: the
/// document only, referencing the user's own files where they already are.
///
/// Plugin state is captured by EngineController on the message thread and
/// arrives here as immutable bytes. The worker writes those bytes beside the
/// journal document, so it never calls into third-party code.

#include "model/Document.hpp"
#include "recovery/RecoverySnapshot.hpp"
#include "recovery/SessionFile.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace daw::recovery {

class RecoveryJournal {
public:
    RecoveryJournal();
    ~RecoveryJournal();

    RecoveryJournal(const RecoveryJournal&) = delete;
    RecoveryJournal& operator=(const RecoveryJournal&) = delete;

    /// Open a session under `root` (the `recovery` directory) and start the
    /// worker thread.
    ///
    /// Returns false when the directory cannot be created. The caller is meant
    /// to carry on regardless: a DAW that will not start because it cannot
    /// write a recovery file is worse than one without recovery.
    bool start(const std::string& root, std::string appVersion,
               std::chrono::milliseconds debounce = std::chrono::seconds(2));

    /// Stop the worker and delete the session directory. This — and only this —
    /// is what marks the session as having ended cleanly.
    void stop();

    bool running() const noexcept { return m_running.load(); }
    const std::string& sessionDir() const noexcept { return m_sessionDir; }

    /// Called from the UI thread after it has captured every live plugin. Takes
    /// ownership of the immutable generation and wakes the worker; a burst of
    /// edits coalesces into one write.
    void requestWrite(RecoverySnapshot snapshot);

    /// Document-only compatibility overload for callers with no live plugins.
    void requestWrite(const ProjectModel& project);

    /// Where the project is saved, so recovery can offer to put the work back
    /// there and can resolve plugin state out of `<package>/state`. Empty until
    /// the user saves for the first time.
    void setProjectPath(std::string path, std::string displayName);

    void setStats(HealthStats stats);

    /// Block until everything requested so far is on disk, bypassing the
    /// debounce. For tests, and for the moment before a deliberate crash.
    void flush();

    /// Journal writes that actually reached the disk. Far below the number of
    /// requests — that difference is the debounce doing its job.
    std::uint64_t writeCount() const noexcept { return m_writeCount.load(); }

private:
    void run(std::chrono::milliseconds debounce);
    void writeSessionFile();

    std::thread m_worker;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;    ///< worker waits here
    std::condition_variable m_written; ///< flush() waits here

    /// The generation waiting to be written, if any. Held by value: the worker
    /// must not reach into a model or plugin the UI thread is still editing.
    std::unique_ptr<RecoverySnapshot> m_pending;
    std::uint64_t m_requested = 0;
    std::uint64_t m_written_ = 0;
    bool m_forceNow = false;
    bool m_stopping = false;

    SessionInfo m_session;
    std::string m_sessionDir;
    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_writeCount{0};
};

} // namespace daw::recovery
