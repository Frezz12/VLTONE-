#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace daw::platform {

/// Project paths carried in std::string are UTF-8.  On Windows a
/// std::filesystem::path is wide, so never let the active ANSI code page make
/// that conversion implicitly.
inline std::filesystem::path pathFromUtf8(std::string_view value) {
#if defined(_WIN32)
    const auto* first = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(first, first + value.size()));
#else
    return std::filesystem::path(value);
#endif
}

/// Convert a native filesystem path back to the UTF-8 convention used by the
/// controller, cache and plugin APIs.
inline std::string pathToUtf8(const std::filesystem::path& value) {
#if defined(_WIN32)
    const std::u8string utf8 = value.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#else
    return value.string();
#endif
}

} // namespace daw::platform
