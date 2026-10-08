#pragma once

#include "model/Document.hpp"
#include "platform/PathUtils.hpp"
#include "Core/Result.hpp"
#include <filesystem>
#include <vector>
#include <cerrno>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif

namespace daw::rendering {

/// Characters a file name cannot carry on one platform or another, plus the
/// ones that would make a name ambiguous in a shell. A track called "Kick / Snr"
/// has to become a file, and it must not become two directories.
inline std::string sanitizeFileName(std::string name) {
    for (char& c : name) {
        const bool illegal = c == '/' || c == '\\' || c == ':' || c == '*' ||
                             c == '?' || c == '"' || c == '<' || c == '>' ||
                             c == '|';
        if (illegal || static_cast<unsigned char>(c) < 0x20) c = '_';
    }
    // Trailing dots and spaces are legal to create on Windows and impossible to
    // delete afterwards.
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    return name.empty() ? std::string("untitled") : name;
}

/// A path nothing else in this render is already writing to. Two tracks are
/// allowed to share a name, and neither should quietly overwrite the other.
inline std::string uniquePath(const std::string& dir, const std::string& stem,
                       std::string_view extension,
                       std::unordered_set<std::string>& taken) {
    for (int attempt = 1;; ++attempt) {
        std::string name = attempt == 1
                               ? stem
                               : stem + " (" + std::to_string(attempt) + ")";
        const std::filesystem::path fileName = platform::pathFromUtf8(
            name + "." + std::string(extension));
        std::string path = platform::pathToUtf8(
            platform::pathFromUtf8(dir) / fileName);
        std::string key = path;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
            return char(std::tolower(c));
        });
        std::error_code ec;
        if (!std::filesystem::exists(platform::pathFromUtf8(path), ec) && !ec &&
            taken.insert(key).second) return path;
        if (ec) throw std::filesystem::filesystem_error("cannot select export path", ec);
    }
}



// Writers only see staging names. Cancellation, exceptions and a failed
// multi-file publication remove this job's files, never earlier exports.
class OutputTransaction {
public:
    ~OutputTransaction() {
        for (const auto& file : m_files) {
            std::error_code ignored;
            std::filesystem::remove(file.temporary, ignored);
            if (!m_committed && file.published)
                std::filesystem::remove(file.destination, ignored);
        }
    }
    std::string stage(const std::string& destination) {
        auto target = platform::pathFromUtf8(destination);
        auto temporary = target;
        temporary += ".partial-" + newUuid();
        m_files.push_back({target, temporary, false});
        return platform::pathToUtf8(temporary);
    }
    audio::Result commit() {
        for (auto& file : m_files) {
            std::error_code ec;
            // Exclusive atomic publication also works on external drives
            // without hard-link support. Never replace a concurrently created
            // output or an input file sharing its name.
#if defined(_WIN32)
            if (!MoveFileW(file.temporary.c_str(), file.destination.c_str()))
                ec = std::error_code(int(GetLastError()), std::system_category());
#elif defined(__APPLE__)
            if (renamex_np(file.temporary.c_str(), file.destination.c_str(), RENAME_EXCL) != 0)
                ec = std::error_code(errno, std::generic_category());
#elif defined(__linux__) && defined(SYS_renameat2)
            if (syscall(SYS_renameat2, AT_FDCWD, file.temporary.c_str(),
                        AT_FDCWD, file.destination.c_str(), 1 /* RENAME_NOREPLACE */) != 0)
                ec = std::error_code(errno, std::generic_category());
#else
            std::filesystem::create_hard_link(file.temporary, file.destination, ec);
#endif
            if (ec) return audio::Result::fail(audio::EngineError::FileWriteError,
                "cannot publish render: " + ec.message());
            file.published = true;
        }
        m_committed = true;
        return audio::Result::ok();
    }
    // The legacy single-file export API promises this exact destination.
    // Replace it atomically only after the writer has closed successfully.
    audio::Result replaceSingle() {
        if (m_files.size() != 1)
            return audio::Result::fail(audio::EngineError::InvalidArgument, "expected one output");
        auto& file = m_files.front();
        std::error_code ec;
#if defined(_WIN32)
        if (!MoveFileExW(file.temporary.c_str(), file.destination.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            ec = std::error_code(int(GetLastError()), std::system_category());
#else
        std::filesystem::rename(file.temporary, file.destination, ec);
#endif
        if (ec) return audio::Result::fail(audio::EngineError::FileWriteError,
                                           "cannot publish render: " + ec.message());
        m_committed = true;
        return audio::Result::ok();
    }
private:
    struct File {
        std::filesystem::path destination, temporary;
        bool published;
    };
    std::vector<File> m_files;
    bool m_committed = false;
};
} // namespace daw::rendering
