#pragma once

#include "Host/PluginInstance.hpp"
#include "plugins/PluginCache.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <stop_token>
#include <thread>
#include <vector>

namespace daw {

/// Collapse the same product advertised through several plugin formats.
///
/// There is no cross-format id shared by AU, VST3, VST and CLAP, so products are
/// matched by their normalised manufacturer/name metadata. The requested
/// format wins when it exists; otherwise the platform's normal fallback order
/// is used. Built-ins and unknown descriptors are always kept verbatim.
std::vector<plugins::PluginDescriptor> preferredPluginVariants(
    std::vector<plugins::PluginDescriptor> descriptors,
    plugins::Format preferredFormat);

/// The same product identity used for format de-duplication in the picker.
bool samePluginProduct(const plugins::PluginDescriptor& a,
                       const plugins::PluginDescriptor& b);

struct ScanOptions {
    /// 0 = min(4, logical CPUs), or two if the CPU count is unknown.
    unsigned maxProcesses = 0;
};

enum class ScanPhase {
    Idle, Collecting, Discovering, Validating, Retrying, Stopping,
    Completed, Cancelled, Failed
};

struct ScanJobInfo {
    std::uint64_t id = 0;
    plugins::Format format = plugins::Format::Unknown;
    std::string path, uid, name;
    bool discovery = false;
    bool retry = false;
    std::chrono::steady_clock::time_point startedAt{};
};

struct ScanOutcome {
    plugins::Format format = plugins::Format::Unknown;
    std::string path, uid, name, reason;
    PluginScanState state = PluginScanState::Pending;
    int attempts = 0;
    std::uint64_t durationMs = 0;
};

struct ScanSnapshot {
    ScanPhase phase = ScanPhase::Idle;
    std::uint32_t filesTotal = 0, filesCompleted = 0;
    std::uint32_t componentsTotal = 0, componentsCompleted = 0;
    std::uint32_t passed = 0, failed = 0;
    std::uint32_t reusedFiles = 0, reusedComponents = 0;
    std::uint32_t discoveries = 0, validations = 0, retries = 0, changedFiles = 0;
    std::uint64_t discoveryMs = 0, validationMs = 0, elapsedMs = 0;
    unsigned maxProcesses = 0;
    bool discoveryComplete = false;
    std::vector<ScanJobInfo> activeJobs; // oldest first
    std::vector<ScanOutcome> results;
    std::string error;
};

/// Search paths, the scan, the cache and the blacklist.
///
/// Framework-agnostic, like the rest of `controller/`: the scan runs on a plain
/// `std::thread` and publishes progress through atomics the UI polls from its
/// existing 33 ms tick. That is the same shape `AudioRecorder` already uses,
/// and it avoids pulling `Qt6::Concurrent` into a project that has never linked
/// it.
class PluginManager {
public:
    explicit PluginManager(std::string cachePath = PluginCache::defaultPath());
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /// Where `daw_scan` lives. Defaults to a sibling of the running executable,
    /// which is where the install rule puts it.
    void setScannerPath(std::string path);
    const std::string& scannerPath() const noexcept { return m_scannerPath; }
    /// Deadline for one attempt; a timeout gets one exclusive retry.
    void setScanTimeout(std::chrono::milliseconds timeout) noexcept {
        m_timeout = timeout;
    }

    void load();
    void copyCatalogFrom(const PluginManager& source) {
        if (&source == this) return;
        const std::scoped_lock lock(m_mutex, source.m_mutex);
        m_cache = source.m_cache;
        m_catalogueRevision.fetch_add(1, std::memory_order_release);
    }
    bool save() const;

    // ── Search paths ──
    std::vector<std::string> searchPaths(plugins::Format format) const;
    void setSearchPaths(plugins::Format format, std::vector<std::string> paths);
    void addSearchPath(plugins::Format format, const std::string& directory);
    void removeSearchPath(plugins::Format format, const std::string& directory);
    /// Whatever the format's factory reports for this platform.
    void resetSearchPathsToDefaults();

    // ── Scanning ──
    /// Returns immediately; the work happens on a worker thread. With
    /// `rescanAll` every file is re-inspected, otherwise a file whose size and
    /// timestamp are unchanged is taken from the cache.
    void startScan(bool rescanAll = false, ScanOptions options = {});
    void cancelScan();
    void waitForScan();
    bool isScanning() const noexcept { return m_scanning.load(std::memory_order_acquire); }

    std::uint32_t scanned() const noexcept { return m_scanned.load(std::memory_order_relaxed); }
    std::uint32_t scanTotal() const noexcept { return m_total.load(std::memory_order_relaxed); }
    float scanProgress() const noexcept;
    ScanSnapshot scanSnapshot() const;
    /// The file being inspected right now, for a status line.
    std::string currentScanPath() const;
    /// A scanner installation or cache write failure, not a plugin failure.
    /// Such errors stop the scan without blacklisting the current plugin.
    std::string lastScanError() const;
    /// True exactly once after a scan finishes, so the UI knows to refresh.
    bool takeScanFinished() noexcept {
        return m_finished.exchange(false, std::memory_order_acq_rel);
    }

    // ── Results ──
    std::vector<plugins::PluginDescriptor> plugins() const;
    std::vector<plugins::PluginDescriptor> effects() const;
    std::vector<plugins::PluginDescriptor> instruments() const;
    void setParameterFingerprintFunction(std::function<std::string(std::string_view)> fingerprint) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_parameterFingerprint = std::move(fingerprint);
        m_catalogueRevision.fetch_add(1, std::memory_order_release);
    }
    /// Stable cache identity for UI presentation models. The instance id
    /// prevents an allocator-reused address from reviving another manager's
    /// menu, while the revision changes whenever visible catalogue data does.
    std::uint64_t instanceId() const noexcept { return m_instanceId; }
    std::uint64_t catalogueRevision() const noexcept {
        return m_catalogueRevision.load(std::memory_order_acquire);
    }
    /// Resolved by identity first and path second, so a plugin that moved on
    /// disk still loads from an old project.
    std::optional<plugins::PluginDescriptor> find(plugins::Format format,
                                                  const std::string& uid) const;

    struct BlacklistEntry {
        plugins::Format format = plugins::Format::Unknown;
        std::string path;
        std::string reason;
        int attempts = 0;
        std::string uid;
        std::string name;
    };
    std::vector<BlacklistEntry> blacklist() const;
    void unblacklist(plugins::Format format, const std::string& path,
                    const std::string& uid = {});
    void clearBlacklist();

    /// Control thread; opens the module, so it may block for a while.
    std::unique_ptr<plugins::PluginInstance> instantiate(
        const plugins::PluginDescriptor& descriptor);
    /// Worker thread only. Runs native state loading in disposable daw_scan;
    /// empty result means success, otherwise a concrete diagnostic. This never
    /// permits a different version or claims portable external sample content.
    std::string probeSharedState(const plugins::PluginDescriptor& descriptor,
        const std::string& absoluteStatePath, double sampleRate = 48000.0) const;
    std::function<std::string()> sharedStateProbe(plugins::PluginDescriptor descriptor,
        std::string absoluteStatePath, double sampleRate = 48000.0) const;

private:
    struct Candidate {
        plugins::Format format;
        std::string path;
    };

    void scanWorker(bool rescanAll, ScanOptions options);
    std::vector<Candidate> collectCandidates() const;
    static std::string defaultScannerPath();

    std::string m_cachePath;
    const std::uint64_t m_instanceId;
    std::string m_scannerPath;
    std::chrono::milliseconds m_timeout{30000};

    /// Guards the cache. The worker thread writes it, the UI thread reads it,
    /// and both do so far too rarely for the lock to matter.
    mutable std::mutex m_mutex;
    PluginCache m_cache;
    std::function<std::string(std::string_view)> m_parameterFingerprint;

    std::thread m_worker;
    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_finished{false};
    std::atomic<std::uint64_t> m_catalogueRevision{0};
    std::atomic<std::uint32_t> m_scanned{0};
    std::atomic<std::uint32_t> m_total{0};
    mutable std::mutex m_currentMutex;
    std::string m_currentPath;
    std::string m_scanError;
    ScanSnapshot m_snapshot;
    std::stop_source m_scanCancellation;
};

} // namespace daw
