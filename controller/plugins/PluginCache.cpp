#include "plugins/PluginCache.hpp"

#include "Scan/ScanProtocol.hpp"
#include "platform/KnownFolders.hpp"
#include "platform/PathUtils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace daw {
namespace fs = std::filesystem;
using json = nlohmann::json;
using plugins::Format;

namespace {

constexpr int kCacheVersion = 2;
std::mutex cacheWriteMutex;

const char* stateName(PluginScanState state) {
    switch (state) {
        case PluginScanState::Passed: return "passed";
        case PluginScanState::Failed: return "failed";
        default: return "pending";
    }
}

PluginScanState readState(const json& value, const char* key) {
    const auto name = value.value(key, std::string("pending"));
    if (name == "passed") return PluginScanState::Passed;
    if (name == "failed") return PluginScanState::Failed;
    if (name != "pending") throw std::runtime_error("invalid scan state");
    return PluginScanState::Pending;
}

bool sameModulePath(const std::string& left, const std::string& right) {
    return PluginCache::normalizedPath(left) == PluginCache::normalizedPath(right);
}

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

bool PluginCacheEntry::complete() const noexcept {
    if (discovery == PluginScanState::Pending) return false;
    if (discovery == PluginScanState::Failed) return true;
    return std::none_of(components.begin(), components.end(), [](const auto& component) {
        return component.state == PluginScanState::Pending;
    });
}

std::string PluginCache::normalizedPath(const std::string& path) {
    std::error_code error;
    auto native = fs::absolute(platform::pathFromUtf8(path), error).lexically_normal();
    if (error) native = platform::pathFromUtf8(path).lexically_normal();
#if defined(_WIN32)
    auto text = native.wstring();
    ::CharLowerBuffW(text.data(), DWORD(text.size()));
    native = text;
#endif
    return platform::pathToUtf8(native);
}

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
    if (!entry.scanStatePresent) {
        // Migration also supports callers that still populate the v1 fields.
        entry.discovery = entry.blacklisted ? PluginScanState::Failed :
                          entry.ok ? PluginScanState::Passed : PluginScanState::Pending;
        if (entry.ok && !entry.blacklisted) {
            for (const auto& descriptor : entry.plugins)
                entry.components.push_back({descriptor.uid, descriptor, PluginScanState::Passed});
        }
        entry.scanStatePresent = true;
    }
    std::sort(entry.components.begin(), entry.components.end(),
        [](const auto& a, const auto& b) { return a.uid < b.uid; });
    std::sort(entry.plugins.begin(), entry.plugins.end(),
        [](const auto& a, const auto& b) { return a.uid < b.uid; });
    for (PluginCacheEntry& existing : m_entries) {
        if (existing.format == entry.format && sameModulePath(existing.path, entry.path)) {
            existing = std::move(entry);
            return;
        }
    }
    m_entries.push_back(std::move(entry));
}

const PluginCacheEntry* PluginCache::find(Format format,
                                          const std::string& path) const {
    for (const PluginCacheEntry& entry : m_entries) {
        if (entry.format == format && sameModulePath(entry.path, path)) return &entry;
    }
    return nullptr;
}

void PluginCache::remove(Format format, const std::string& path) {
    std::erase_if(m_entries, [&](const PluginCacheEntry& entry) {
        return entry.format == format && sameModulePath(entry.path, path);
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
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        if (a.format != b.format) return a.format < b.format;
        if (a.path != b.path) return a.path < b.path;
        return a.uid < b.uid;
    });
    return found;
}

bool PluginCache::load(const std::string& path) {
    m_entries.clear();
    m_searchPathsInitialized = false;
    m_vstPathsPresent = false;
    m_clapPaths.clear(); m_vst3Paths.clear(); m_vstPaths.clear(); m_auPaths.clear();
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
    try {
        if (!root.is_object()) return false;
        const int version = root.value("version", 0);
        if (version != 1 && version != kCacheVersion) return false;
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
            try {
                PluginCacheEntry entry;
                entry.format = plugins::formatFromString(value.value("format", std::string()));
                entry.path = value.value("path", std::string());
                entry.fileSize = value.value("fileSize", std::uint64_t(0));
                entry.fileModifiedTime = value.value("fileModifiedTime", std::int64_t(0));
                entry.schemaVersion = value.value("schema", 0);
                entry.ok = value.value("ok", false);
                entry.blacklisted = value.value("blacklisted", false);
                entry.scannerVerified = value.value("scannerVerified", false);
                entry.failureReason = value.value("reason", std::string());
                entry.attempts = value.value("attempts", 0);
                if (entry.format == Format::Unknown || entry.path.empty()) continue;

                if (value.contains("plugins") && value["plugins"].is_array()) {
                    for (const json& descriptor : value["plugins"]) {
                        auto parsed = descriptorFromJson(descriptor);
                        if (parsed.format == entry.format && !parsed.uid.empty() &&
                            sameModulePath(parsed.path, entry.path))
                            entry.plugins.push_back(std::move(parsed));
                    }
                }
                if (version == 2) {
                    entry.scanStatePresent = true;
                    entry.discovery = readState(value, "discovery");
                    entry.discoveryDurationMs = value.value("discoveryMs", std::uint64_t(0));
                    entry.timeoutRetryPending = value.value("timeoutRetryPending", false);
                    if (!value.contains("components") || !value["components"].is_array()) continue;
                    for (const auto& item : value["components"]) {
                        PluginComponentResult component;
                        component.uid = item.at("uid").get<std::string>();
                        component.descriptor = descriptorFromJson(item.at("descriptor"));
                        if (component.uid.empty() || component.uid != component.descriptor.uid ||
                            component.descriptor.format != entry.format ||
                            !sameModulePath(component.descriptor.path, entry.path))
                            throw std::runtime_error("invalid cached component");
                        component.state = readState(item, "state");
                        component.failureReason = item.value("reason", std::string());
                        component.attempts = std::max(0, item.value("attempts", 0));
                        component.durationMs = item.value("durationMs", std::uint64_t(0));
                        component.timeoutRetryPending = item.value("timeoutRetryPending", false);
                        if (std::any_of(entry.components.begin(), entry.components.end(),
                            [&](const auto& c) { return c.uid == component.uid; }))
                            throw std::runtime_error("duplicate cached UID");
                        entry.components.push_back(std::move(component));
                    }
                }
                put(std::move(entry));
            } catch (const std::exception&) { /* A malformed module needs a new scan. */ }
        }
        return true;
    } catch (const std::exception&) { return false; }
}

bool PluginCache::save(const std::string& path) const {
    std::lock_guard writeLock(cacheWriteMutex);
    try {
        std::error_code ec;
        const fs::path target = platform::pathFromUtf8(path);
        if (!target.parent_path().empty()) fs::create_directories(target.parent_path(), ec);
        if (ec) return false;

        json entries = json::array();
        for (const PluginCacheEntry& entry : m_entries) {
            json components = json::array();
            for (const auto& component : entry.components) {
                components.push_back({{"uid", component.uid},
                    {"descriptor", descriptorToJson(component.descriptor)},
                    {"state", stateName(component.state)}, {"reason", component.failureReason},
                    {"attempts", component.attempts}, {"durationMs", component.durationMs},
                    {"timeoutRetryPending", component.timeoutRetryPending}});
            }
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
                {"scannerVerified", entry.scannerVerified},
                {"reason", entry.failureReason},
                {"attempts", entry.attempts},
                {"plugins", descriptors},
                {"discovery", stateName(entry.discovery)},
                {"discoveryMs", entry.discoveryDurationMs},
                {"timeoutRetryPending", entry.timeoutRetryPending},
                {"components", components},
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
        // Plugin stderr may contain locale-encoded or binary bytes. Diagnostic
        // text must never turn a plugin failure into a cache-write failure.
        os << root.dump(2, ' ', false, json::error_handler_t::replace);
        os.flush();
        if (!os.good()) {
            os.close();
            fs::remove(temporary, ec);
            return false;
        }
        os.close();
        if (os.fail()) { fs::remove(temporary, ec); return false; }
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
    } catch (const std::exception&) { return false; }
}

} // namespace daw
