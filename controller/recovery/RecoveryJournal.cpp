#include "recovery/RecoveryJournal.hpp"

#include "ProjectSerializer.hpp"
#include "platform/PathUtils.hpp"

#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace daw::recovery {

namespace {

/// How often the heartbeat is refreshed. The guard calls a session hung when
/// this stops advancing for several times this long, so it wants to be short
/// enough to notice a freeze and long enough to cost nothing.
constexpr auto kHeartbeatInterval = std::chrono::seconds(1);

bool writeStateFile(const fs::path& stateDir,
                    const RecoverySnapshot::PluginState& state) {
    const fs::path fileName = platform::pathFromUtf8(state.fileName);
    if (fileName.empty() || fileName != fileName.filename() || state.bytes.empty())
        return false;

    std::error_code ec;
    const fs::path target = stateDir / fileName;
    if (fs::is_regular_file(target, ec) && !ec) return true;

    ec.clear();
    fs::path temporary = target;
    temporary += ".tmp-" + newUuid();
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(state.bytes.data()),
                 std::streamsize(state.bytes.size()));
    stream.flush();
    if (!stream.good()) {
        stream.close();
        fs::remove(temporary, ec);
        return false;
    }
    stream.close();
    fs::rename(temporary, target, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

bool writeSnapshot(const RecoverySnapshot& snapshot, const fs::path& journalFile,
                   const fs::path& sessionDir) {
    const fs::path stateDir = sessionDir / ProjectSerializer::kStateDir;
    std::error_code ec;
    if (!snapshot.pluginStates.empty()) {
        fs::create_directories(stateDir, ec);
        if (ec) return false;
    }

    std::set<std::string> currentFiles;
    for (const RecoverySnapshot::PluginState& state : snapshot.pluginStates) {
        if (!writeStateFile(stateDir, state)) return false;
        currentFiles.insert(state.fileName);
    }

    // Publish the manifest only after every state file it names is written. The
    // previous manifest and its files remain a valid generation if any write
    // above fails or the process dies halfway through.
    if (!ProjectSerializer::saveDocument(snapshot.project,
                                         platform::pathToUtf8(journalFile),
                                         MediaPaths::Absolute)) {
        return false;
    }

    // Now that the new manifest is atomically visible, old chunks are debris.
    ec.clear();
    if (fs::is_directory(stateDir, ec) && !ec) {
        for (const auto& entry : fs::directory_iterator(stateDir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const std::string name = platform::pathToUtf8(entry.path().filename());
            if (!currentFiles.contains(name)) fs::remove(entry.path(), ec);
        }
        ec.clear();
        if (currentFiles.empty()) fs::remove(stateDir, ec);
    }
    return true;
}

} // namespace

RecoveryJournal::RecoveryJournal() = default;

RecoveryJournal::~RecoveryJournal() {
    // Deliberately NOT stop(): a destructor running during a crash unwind must
    // not delete the session directory, which is exactly the evidence recovery
    // needs. Only an explicit stop() marks a clean shutdown.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
    m_running.store(false);
}

bool RecoveryJournal::start(const std::string& root, std::string appVersion,
                            std::chrono::milliseconds debounce) {
    if (m_running.load()) return true;

    std::error_code ec;
    fs::create_directories(platform::pathFromUtf8(root), ec);
    if (ec) return false;

    m_session = SessionInfo{};
    m_session.pid = currentProcessId();
    m_session.appVersion = std::move(appVersion);
    m_session.startedUnixMs = nowUnixMs();

    // pid plus start time: two DAWs running at once get separate directories,
    // and a recycled pid cannot collide with an old session either.
    const std::string name = std::to_string(m_session.pid) + "-" +
                             std::to_string(m_session.startedUnixMs);
    const fs::path directory = platform::pathFromUtf8(root) / name;
    fs::create_directories(directory, ec);
    if (ec) return false;

    m_sessionDir = platform::pathToUtf8(directory);
    m_session.directory = m_sessionDir;
    if (!writeSession(m_session)) {
        fs::remove_all(directory, ec);
        m_sessionDir.clear();
        return false;
    }

    m_stopping = false;
    m_running.store(true);
    m_worker = std::thread([this, debounce] { run(debounce); });
    return true;
}

void RecoveryJournal::stop() {
    if (!m_running.load()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
    m_running.store(false);

    // The directory's absence IS the clean-shutdown flag.
    discardSession(m_sessionDir);
    m_sessionDir.clear();
}

void RecoveryJournal::requestWrite(RecoverySnapshot snapshot) {
    if (!m_running.load()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Replacing an unwritten pending model is the point: only the newest
        // state matters, and a burst of edits costs one write, not one each.
        m_pending = std::make_unique<RecoverySnapshot>(std::move(snapshot));
        ++m_requested;
    }
    m_wake.notify_all();
}

void RecoveryJournal::requestWrite(const ProjectModel& project) {
    if (!m_running.load()) return;
    RecoverySnapshot snapshot;
    snapshot.project = project;
    requestWrite(std::move(snapshot));
}

void RecoveryJournal::setProjectPath(std::string path, std::string displayName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_session.projectPath = std::move(path);
    m_session.projectName = std::move(displayName);
}

void RecoveryJournal::setStats(HealthStats stats) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_session.stats = std::move(stats);
}

void RecoveryJournal::flush() {
    if (!m_running.load()) return;
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_written_ >= m_requested) return;
    m_forceNow = true;
    m_wake.notify_all();
    m_written.wait(lock, [this] { return m_written_ >= m_requested || m_stopping; });
}

void RecoveryJournal::writeSessionFile() {
    // Called with the lock held; copying is cheap and keeps the file write
    // itself outside the critical section in the caller's flow.
    SessionInfo snapshot = m_session;
    snapshot.heartbeat = m_session.heartbeat;
    writeSession(snapshot);
}

void RecoveryJournal::run(std::chrono::milliseconds debounce) {
    using Clock = std::chrono::steady_clock;
    const fs::path journalFile = platform::pathFromUtf8(m_sessionDir) / kJournalFile;

    auto lastWrite = Clock::now() - debounce;   // the first request writes at once
    auto lastHeartbeat = Clock::time_point{};

    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_stopping) {
        const auto now = Clock::now();

        // ── the journal ──
        if (m_pending && (m_forceNow || now - lastWrite >= debounce)) {
            std::unique_ptr<RecoverySnapshot> snapshot = std::move(m_pending);
            const std::uint64_t generation = m_requested;
            m_forceNow = false;
            lock.unlock();

            const bool ok = writeSnapshot(*snapshot, journalFile,
                                          platform::pathFromUtf8(m_sessionDir));
            snapshot.reset();   // free the copy outside the lock

            lock.lock();
            lastWrite = Clock::now();
            if (ok) {
                m_session.journalUnixMs = nowUnixMs();
                m_writeCount.fetch_add(1);
            }
            // The generation advances either way. A journal that cannot be
            // written will not start writing because a caller waits on it, and
            // flush() must not deadlock on a full disk.
            m_written_ = generation;
            // Refresh the session file too, so its journalUnixMs and statistics
            // describe the write that just happened rather than the previous
            // heartbeat. A crash straight after a flush must not leave a
            // session that disagrees with its own journal.
            writeSessionFile();
            m_written.notify_all();
        }

        // ── the heartbeat the guard watches ──
        if (now - lastHeartbeat >= kHeartbeatInterval) {
            ++m_session.heartbeat;
            m_session.heartbeatUnixMs = nowUnixMs();
            writeSessionFile();
            lastHeartbeat = now;
        }

        // Wake for whichever comes first: the heartbeat, or the moment the
        // debounce lets a pending write through.
        auto deadline = lastHeartbeat + kHeartbeatInterval;
        if (m_pending) deadline = std::min(deadline, lastWrite + debounce);
        m_wake.wait_until(lock, deadline, [this] { return m_stopping || m_forceNow; });
    }

    // A pending edit at shutdown still gets written: stop() deletes the
    // directory afterwards, but a crash-time join does not.
    if (m_pending) {
        std::unique_ptr<RecoverySnapshot> snapshot = std::move(m_pending);
        const std::uint64_t generation = m_requested;
        lock.unlock();
        const bool ok = writeSnapshot(*snapshot, journalFile,
                                      platform::pathFromUtf8(m_sessionDir));
        snapshot.reset();
        lock.lock();
        if (ok) m_writeCount.fetch_add(1);
        m_written_ = generation;
    }
    m_written.notify_all();
}

} // namespace daw::recovery
