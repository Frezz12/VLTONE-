#pragma once

#include <filesystem>

namespace daw::platform {

enum class KnownFolder {
    Profile,
    RoamingAppData,
    LocalAppData,
    CommonProgramFiles,
};

/// Resolve an OS-owned folder without reading a localized/ANSI environment
/// variable. Returns an empty path only when the operating system has no
/// usable answer.
std::filesystem::path knownFolderPath(KnownFolder folder);

} // namespace daw::platform
