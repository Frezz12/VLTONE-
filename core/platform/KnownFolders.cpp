#include "platform/KnownFolders.hpp"

#include "platform/PathUtils.hpp"

#include <cstdlib>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif

namespace daw::platform {

std::filesystem::path knownFolderPath(KnownFolder folder) {
#if defined(_WIN32)
    const KNOWNFOLDERID* id = nullptr;
    switch (folder) {
        case KnownFolder::Profile: id = &FOLDERID_Profile; break;
        case KnownFolder::RoamingAppData: id = &FOLDERID_RoamingAppData; break;
        case KnownFolder::LocalAppData: id = &FOLDERID_LocalAppData; break;
        case KnownFolder::CommonProgramFiles:
            id = &FOLDERID_ProgramFilesCommon;
            break;
    }

    PWSTR value = nullptr;
    if (!id || FAILED(::SHGetKnownFolderPath(*id, KF_FLAG_DEFAULT, nullptr,
                                             &value))) {
        return {};
    }
    const std::filesystem::path result(value);
    ::CoTaskMemFree(value);
    return result;
#else
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    const std::filesystem::path profile = pathFromUtf8(home);
    switch (folder) {
        case KnownFolder::Profile: return profile;
        case KnownFolder::RoamingAppData:
        case KnownFolder::LocalAppData: return profile / ".config";
        case KnownFolder::CommonProgramFiles: return {};
    }
    return {};
#endif
}

} // namespace daw::platform
