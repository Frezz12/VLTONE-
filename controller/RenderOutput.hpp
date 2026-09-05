#pragma once

#include "model/Document.hpp"
#include "platform/PathUtils.hpp"
#include "Core/Result.hpp"
#include <filesystem>
#include <vector>
#include <cerrno>
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
