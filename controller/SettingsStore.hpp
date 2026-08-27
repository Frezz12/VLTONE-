#pragma once

#include <string>
#include <unordered_map>

namespace daw {

/// Portable application settings (device UID, sample rate, buffer size, window
/// state, …). Replaces the Swift UserDefaults usage. Values are stored as
/// strings and persisted as JSON; typed accessors convert on the way in/out.
/// Kept nlohmann-free in the header so UI code doesn't pull the JSON library.
class SettingsStore {
public:
    /// Construct against an explicit file, or the per-user default location.
    explicit SettingsStore(std::string filePath = defaultPath());

    /// Default per-user settings file (~/.config/VLT Studio Pro/settings.json, or the
    /// the Windows Roaming AppData Known Folder equivalent).
    static std::string defaultPath();

    void load();
    bool save() const;

    std::string getString(const std::string& key,
                          const std::string& fallback = "") const;
    int getInt(const std::string& key, int fallback = 0) const;
    double getDouble(const std::string& key, double fallback = 0.0) const;
    bool getBool(const std::string& key, bool fallback = false) const;

    void setString(const std::string& key, const std::string& value);
    void setInt(const std::string& key, int value);
    void setDouble(const std::string& key, double value);
    void setBool(const std::string& key, bool value);

    bool has(const std::string& key) const { return m_values.count(key) != 0; }
    void remove(const std::string& key) { m_values.erase(key); }

private:
    std::string m_path;
    std::unordered_map<std::string, std::string> m_values;
};

} // namespace daw
