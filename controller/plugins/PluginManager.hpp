#pragma once

#include "Host/PluginInstance.hpp"
#include "plugins/PluginCache.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
    /// How long one plugin gets before it is killed and blacklisted.
    void setScanTimeout(std::chrono::milliseconds timeout) noexcept {
        m_timeout = timeout;
    }

    void load();
    void copyCatalogFrom(const PluginManager& source) {
        if (&source == this) return;
        const std::scoped_lock lock(m_mutex, source.m_mutex);
        m_cache = source.m_cache;
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
    void startScan(bool rescanAll = false);
    void cancelScan();
    void waitForScan();
    bool isScanning() const noexcept { return m_scanning.load(std::memory_order_acquire); }

    std::uint32_t scanned() const noexcept { return m_scanned.load(std::memory_order_relaxed); }
    std::uint32_t scanTotal() const noexcept { return m_total.load(std::memory_order_relaxed); }
    float scanProgress() const noexcept;
    /// The file being inspected right now, for a status line.
    std::string currentScanPath() const;
    /// True exactly once after a scan finishes, so the UI knows to refresh.
    bool takeScanFinished() noexcept {
        return m_finished.exchange(false, std::memory_order_acq_rel);
    }

    // ── Results ──
    std::vector<plugins::PluginDescriptor> plugins() const;
    std::vector<plugins::PluginDescriptor> effects() const;
    std::vector<plugins::PluginDescriptor> instruments() const;
    /// Resolved by identity first and path second, so a plugin that moved on
    /// disk still loads from an old project.
    std::optional<plugins::PluginDescriptor> find(plugins::Format format,
                                                  const std::string& uid) const;

    struct BlacklistEntry {
        plugins::Format format = plugins::Format::Unknown;
        std::string path;
        std::string reason;
        int attempts = 0;
    };
    std::vector<BlacklistEntry> blacklist() const;
    void unblacklist(plugins::Format format, const std::string& path);
    void clearBlacklist();

    /// Control thread; opens the module, so it may block for a while.
    std::unique_ptr<plugins::PluginInstance> instantiate(
        const plugins::PluginDescriptor& descriptor);

private:
    struct Candidate {
        plugins::Format format;
        std::string path;
    };

    void scanWorker(bool rescanAll);
    std::vector<Candidate> collectCandidates() const;
    static std::string defaultScannerPath();

    std::string m_cachePath;
    std::string m_scannerPath;
    std::chrono::milliseconds m_timeout{30000};

    /// Guards the cache. The worker thread writes it, the UI thread reads it,
    /// and both do so far too rarely for the lock to matter.
    mutable std::mutex m_mutex;
    PluginCache m_cache;

    std::thread m_worker;
    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_finished{false};
    std::atomic<std::uint32_t> m_scanned{0};
    std::atomic<std::uint32_t> m_total{0};
    mutable std::mutex m_currentMutex;
    std::string m_currentPath;
};

} // namespace daw
