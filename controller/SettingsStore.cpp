#include "SettingsStore.hpp"
#include "platform/KnownFolders.hpp"
#include "platform/PathUtils.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace daw {

std::string SettingsStore::defaultPath() {
    fs::path dir = platform::knownFolderPath(platform::KnownFolder::RoamingAppData);
    if (dir.empty()) dir = fs::temp_directory_path();
    return platform::pathToUtf8(dir / "VLT Studio Pro" / "settings.json");
}

SettingsStore::SettingsStore(std::string filePath)
    : m_path(std::move(filePath)) {
    load();
}

void SettingsStore::load() {
    m_values.clear();
    std::ifstream is(platform::pathFromUtf8(m_path));
    if (!is) return;
    json j;
    try {
        is >> j;
    } catch (...) {
        return; // corrupt file — start fresh
    }
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_string()) {
            m_values[it.key()] = it.value().get<std::string>();
        }
    }
}

bool SettingsStore::save() const {
    std::error_code ec;
    const fs::path nativePath = platform::pathFromUtf8(m_path);
    fs::create_directories(nativePath.parent_path(), ec);
    json j = json::object();
    for (const auto& [k, v] : m_values) j[k] = v;
    std::ofstream os(nativePath);
    if (!os) return false;
    os << j.dump(2);
    return os.good();
}

std::string SettingsStore::getString(const std::string& key,
                                     const std::string& fallback) const {
    auto it = m_values.find(key);
    return it == m_values.end() ? fallback : it->second;
}

int SettingsStore::getInt(const std::string& key, int fallback) const {
    auto it = m_values.find(key);
    if (it == m_values.end()) return fallback;
    try { return std::stoi(it->second); } catch (...) { return fallback; }
}

double SettingsStore::getDouble(const std::string& key, double fallback) const {
    auto it = m_values.find(key);
    if (it == m_values.end()) return fallback;
    try { return std::stod(it->second); } catch (...) { return fallback; }
}

bool SettingsStore::getBool(const std::string& key, bool fallback) const {
    auto it = m_values.find(key);
    if (it == m_values.end()) return fallback;
    return it->second == "true" || it->second == "1";
}

void SettingsStore::setString(const std::string& key, const std::string& value) {
    m_values[key] = value;
}
void SettingsStore::setInt(const std::string& key, int value) {
    m_values[key] = std::to_string(value);
}
void SettingsStore::setDouble(const std::string& key, double value) {
    m_values[key] = std::to_string(value);
}
void SettingsStore::setBool(const std::string& key, bool value) {
    m_values[key] = value ? "true" : "false";
}

} // namespace daw
