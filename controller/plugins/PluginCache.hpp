#pragma once

#include "Host/PluginTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

/// One scanned file and what came out of it.
///
/// A failed entry is as valuable as a successful one: it is what stops the DAW
/// re-launching the scanner on a plugin that crashed it every time the user
/// opens the manager.
struct PluginCacheEntry {
    plugins::Format format = plugins::Format::Unknown;
    std::string path;
    /// Identity of the file as scanned. A rescan reuses the entry only when all
    /// of these still match, which is what makes an incremental scan safe.
    std::uint64_t fileSize = 0;
    std::int64_t fileModifiedTime = 0;
    int schemaVersion = 0;

    bool ok = false;
    bool blacklisted = false;
    std::string failureReason;
    int attempts = 0;

    std::vector<plugins::PluginDescriptor> plugins;
};

/// The scan results, on disk.
///
/// A file of its own — `~/.config/VLT Studio Pro/plugins.json`, beside `settings.json` —
/// rather than a corner of `SettingsStore`, which stores flat string→string
/// pairs and silently drops anything nested. This is machine-scoped scan
/// output, not user preference, and the two age differently: deleting it costs
/// a rescan, deleting settings costs the user their setup.
class PluginCache {
public:
    /// `~/.config/VLT Studio Pro/plugins.json`, or the Windows Roaming AppData
    /// Known Folder under `VLT Studio Pro\plugins.json`.
    static std::string defaultPath();

    /// Missing or corrupt reads as empty — a broken cache must cost a rescan,
    /// never a failure to start.
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    const std::vector<std::string>& searchPaths(plugins::Format format) const;
    void setSearchPaths(plugins::Format format, std::vector<std::string> paths);
    bool searchPathsInitialized() const noexcept { return m_searchPathsInitialized; }
    void markSearchPathsInitialized() noexcept { m_searchPathsInitialized = true; }
    /// Distinguishes an intentionally empty VST path list from a pre-VST cache
    /// that simply has no `searchPaths.vst` key yet.
    bool vstSearchPathsPresent() const noexcept { return m_vstPathsPresent; }

    const std::vector<PluginCacheEntry>& entries() const noexcept { return m_entries; }
    /// Replaces the entry with the same (format, path), or appends.
    void put(PluginCacheEntry entry);
    const PluginCacheEntry* find(plugins::Format format, const std::string& path) const;
    void remove(plugins::Format format, const std::string& path);
    void clear();

    /// True when a cached entry can stand in for a fresh scan of this file.
    static bool isCurrent(const PluginCacheEntry& entry, std::uint64_t fileSize,
                          std::int64_t fileModifiedTime) noexcept;

    /// Every descriptor from every successful entry, flattened.
    std::vector<plugins::PluginDescriptor> allPlugins() const;

private:
    std::vector<PluginCacheEntry> m_entries;
    std::vector<std::string> m_clapPaths;
    std::vector<std::string> m_vst3Paths;
    std::vector<std::string> m_vstPaths;
    std::vector<std::string> m_auPaths;
    std::vector<std::string> m_empty;
    bool m_searchPathsInitialized = false;
    bool m_vstPathsPresent = false;
};

} // namespace daw
