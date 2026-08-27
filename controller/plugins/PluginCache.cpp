#include "plugins/PluginCache.hpp"

#include "Scan/ScanProtocol.hpp"
#include "platform/KnownFolders.hpp"
#include "platform/PathUtils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace daw {
namespace fs = std::filesystem;
using json = nlohmann::json;
using plugins::Format;

namespace {

constexpr int kCacheVersion = 1;

json descriptorToJson(const plugins::PluginDescriptor& descriptor) {
    // Round-trips through the scanner's own encoder, so the cache and the wire
    // can never describe a plugin differently.
    return json::parse(plugins::scan::descriptorToJson(descriptor));
}

plugins::PluginDescriptor descriptorFromJson(const json& value) {
    std::vector<plugins::PluginDescriptor> parsed;
    const json wrapper{{"schema", plugins::scan::kSchemaVersion},
                       {"plugins", json::array({value})}};
    plugins::scan::decodeResult(wrapper.dump(), parsed);
    return parsed.empty() ? plugins::PluginDescriptor{} : parsed.front();
}

json pathsToJson(const std::vector<std::string>& paths) {
    json array = json::array();
    for (const std::string& path : paths) array.push_back(path);
    return array;
}

std::vector<std::string> pathsFromJson(const json& parent, const char* key) {
    std::vector<std::string> paths;
    if (!parent.contains(key) || !parent[key].is_array()) return paths;
    for (const json& entry : parent[key]) {
        if (entry.is_string()) paths.push_back(entry.get<std::string>());
    }
    return paths;
}

} // namespace

std::string PluginCache::defaultPath() {
    fs::path dir = platform::knownFolderPath(platform::KnownFolder::RoamingAppData);
    if (dir.empty()) dir = fs::temp_directory_path();
    return platform::pathToUtf8(dir / "VLT Studio Pro" / "plugins.json");
}

bool PluginCache::isCurrent(const PluginCacheEntry& entry, std::uint64_t fileSize,
                            std::int64_t fileModifiedTime) noexcept {
    // The schema check is what forces a rescan when the descriptor gains a
    // field, without the user ever having to know to ask for one.
    return entry.schemaVersion == plugins::scan::kSchemaVersion &&
           entry.fileSize == fileSize && entry.fileModifiedTime == fileModifiedTime;
}

const std::vector<std::string>& PluginCache::searchPaths(Format format) const {
    switch (format) {
        case Format::Clap: return m_clapPaths;
        case Format::Vst3: return m_vst3Paths;
        case Format::Vst: return m_vstPaths;
        case Format::AudioUnit: return m_auPaths;
        case Format::Internal: return m_empty;
        case Format::Unknown: break;
    }
    return m_empty;
}

void PluginCache::setSearchPaths(Format format, std::vector<std::string> paths) {
    switch (format) {
        case Format::Clap: m_clapPaths = std::move(paths); break;
        case Format::Vst3: m_vst3Paths = std::move(paths); break;
        case Format::Vst:
            m_vstPaths = std::move(paths);
            m_vstPathsPresent = true;
            break;
        case Format::AudioUnit: m_auPaths = std::move(paths); break;
        case Format::Internal: break;
        case Format::Unknown: break;
    }
}

void PluginCache::put(PluginCacheEntry entry) {
    for (PluginCacheEntry& existing : m_entries) {
        if (existing.format == entry.format && existing.path == entry.path) {
            existing = std::move(entry);
            return;
        }
    }
    m_entries.push_back(std::move(entry));
}

const PluginCacheEntry* PluginCache::find(Format format,
                                          const std::string& path) const {
    for (const PluginCacheEntry& entry : m_entries) {
        if (entry.format == format && entry.path == path) return &entry;
    }
    return nullptr;
}

void PluginCache::remove(Format format, const std::string& path) {
    std::erase_if(m_entries, [&](const PluginCacheEntry& entry) {
        return entry.format == format && entry.path == path;
    });
}

void PluginCache::clear() {
    m_entries.clear();
}

std::vector<plugins::PluginDescriptor> PluginCache::allPlugins() const {
    std::vector<plugins::PluginDescriptor> found;
    for (const PluginCacheEntry& entry : m_entries) {
        if (!entry.ok || entry.blacklisted) continue;
        found.insert(found.end(), entry.plugins.begin(), entry.plugins.end());
    }
    return found;
}

bool PluginCache::load(const std::string& path) {
    m_entries.clear();
    m_searchPathsInitialized = false;
    m_vstPathsPresent = false;
    std::ifstream is(platform::pathFromUtf8(path));
    if (!is) return false;

    json root;
    try {
        is >> root;
    } catch (const std::exception&) {
        // A truncated or hand-edited cache costs a rescan, not a failure to
        // start. There is nothing here that cannot be regenerated.
        return false;
    }
    if (!root.is_object() || root.value("version", 0) != kCacheVersion) return false;
    m_searchPathsInitialized = root.value("searchPathsInitialized", false);

    if (root.contains("searchPaths") && root["searchPaths"].is_object()) {
        const json& paths = root["searchPaths"];
        m_clapPaths = pathsFromJson(paths, "clap");
        m_vst3Paths = pathsFromJson(paths, "vst3");
        m_vstPaths = pathsFromJson(paths, "vst");
        m_vstPathsPresent = paths.contains("vst");
        m_auPaths = pathsFromJson(paths, "au");
    }

    if (!root.contains("entries") || !root["entries"].is_array()) return true;
    for (const json& value : root["entries"]) {
        if (!value.is_object()) continue;
        PluginCacheEntry entry;
        entry.format = plugins::formatFromString(value.value("format", std::string()));
        entry.path = value.value("path", std::string());
        entry.fileSize = value.value("fileSize", std::uint64_t(0));
        entry.fileModifiedTime = value.value("fileModifiedTime", std::int64_t(0));
        entry.schemaVersion = value.value("schema", 0);
        entry.ok = value.value("ok", false);
        entry.blacklisted = value.value("blacklisted", false);
        entry.failureReason = value.value("reason", std::string());
        entry.attempts = value.value("attempts", 0);
        if (entry.format == Format::Unknown || entry.path.empty()) continue;

        if (value.contains("plugins") && value["plugins"].is_array()) {
            for (const json& descriptor : value["plugins"]) {
                entry.plugins.push_back(descriptorFromJson(descriptor));
            }
        }
        m_entries.push_back(std::move(entry));
    }
    return true;
}

bool PluginCache::save(const std::string& path) const {
    std::error_code ec;
    const fs::path target = platform::pathFromUtf8(path);
    fs::create_directories(target.parent_path(), ec);

    json entries = json::array();
    for (const PluginCacheEntry& entry : m_entries) {
        json descriptors = json::array();
        for (const plugins::PluginDescriptor& descriptor : entry.plugins) {
            descriptors.push_back(descriptorToJson(descriptor));
        }
        entries.push_back(json{
            {"format", std::string(plugins::toString(entry.format))},
            {"path", entry.path},
            {"fileSize", entry.fileSize},
            {"fileModifiedTime", entry.fileModifiedTime},
            {"schema", entry.schemaVersion},
            {"ok", entry.ok},
            {"blacklisted", entry.blacklisted},
            {"reason", entry.failureReason},
            {"attempts", entry.attempts},
            {"plugins", descriptors},
        });
    }

    json root{
        {"version", kCacheVersion},
        {"searchPathsInitialized", m_searchPathsInitialized},
        {"searchPaths",
         json{{"clap", pathsToJson(m_clapPaths)},
              {"vst3", pathsToJson(m_vst3Paths)},
              {"vst", pathsToJson(m_vstPaths)},
              {"au", pathsToJson(m_auPaths)}}},
        {"entries", entries},
    };

    fs::path temporary = target;
    temporary += ".tmp";
    fs::remove(temporary, ec);
    ec.clear();
    std::ofstream os(temporary, std::ios::trunc);
    if (!os) return false;
    // Pretty-printed, unlike the wire format: this one a user may well open to
    // find out why their plugin is not showing up.
    os << root.dump(2);
    os.flush();
    if (!os.good()) {
        os.close();
        fs::remove(temporary, ec);
        return false;
    }
    os.close();
#if defined(_WIN32)
    const bool replaced = ::MoveFileExW(temporary.wstring().c_str(),
                                        target.wstring().c_str(),
                                        MOVEFILE_REPLACE_EXISTING |
                                            MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!replaced) {
        fs::remove(temporary, ec);
        return false;
    }
#else
    fs::rename(temporary, target, ec);
    if (ec) {
        fs::remove(temporary, ec);
        return false;
    }
#endif
    return true;
}

} // namespace daw
