#pragma once

#include "Host/PluginTypes.hpp"

#include <QString>

namespace ui {

inline constexpr const char* kPreferredPluginFormatSetting =
    "plugins/preferredFormat";

/// AU is the native default on macOS; VST3 is the portable default elsewhere
/// (and specifically on Windows). A missing preferred variant still falls back
/// to every other compiled format in `preferredPluginVariants`.
daw::plugins::Format defaultPreferredPluginFormat() noexcept;
daw::plugins::Format preferredPluginFormat();
QString pluginFormatSettingValue(daw::plugins::Format format);

} // namespace ui
