#pragma once

#include <QString>
#include <QStringList>

namespace ui::channelstrippresets {

inline constexpr const char* kExtension = "vlts";

/// The browser-visible preset root in the application's local data folder.
/// Calling this ensures both `Presets` and `Presets/Channel Strips` exist.
QString rootFolder();
QString stripFolder();
/// Presets used only by Direct Offline Processing. They deliberately share the
/// portable VLTS format but live in their own browser-neutral folder.
QString offlineFolder();
QStringList offlineFiles();
QString offlineFilePathForName(const QString& name);

/// Saved Channel Strip presets, ordered by their display names.
QStringList files();
QString displayName(const QString& filePath);

/// Resolve a user-entered name inside the managed folder. Empty means the name
/// is unsafe on macOS/Windows (path separators, reserved punctuation, etc.).
QString filePathForName(const QString& name);

bool isPresetFile(const QString& path);

} // namespace ui::channelstrippresets
