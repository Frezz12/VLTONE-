#include "plugins/ScanProcess.hpp"
#include "platform/PathUtils.hpp"

#include <array>
#include <cstring>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#include <thread>
#else
#include <csignal>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace daw {
namespace {

using Clock = std::chrono::steady_clock;

std::string describeExit(int exitCode) {
    return "exited with code " + std::to_string(exitCode);
}

#if defined(_WIN32)

std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                              static_cast<int>(value.size()), result.data(),
                              length) != length) {
        return {};
    }
    return result;
}

/// Quote one argv element according to CommandLineToArgvW/MSVC parsing rules.
/// Backslashes only need doubling when they precede a quote or the closing
/// quote; this is the corner case naive `"argument"` construction misses.
std::wstring quoteWindowsArgument(std::wstring_view argument) {
    const bool needsQuotes =
        argument.empty() ||
        argument.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needsQuotes) return std::wstring(argument);

    std::wstring result(1, L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(character);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring makeWindowsCommandLine(
    const std::wstring& executable,
    const std::vector<std::string>& arguments) {
    std::wstring commandLine = quoteWindowsArgument(executable);
    for (const std::string& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(utf8ToWide(argument));
    }
    return commandLine;
}

#endif

} // namespace

#if !defined(_WIN32)

ScanProcessResult ScanProcess::run(const std::string& executable,
                                   const std::vector<std::string>& arguments,
                                   std::chrono::milliseconds timeout) {
    ScanProcessResult result;

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) {
        result.failureReason = "could not create a pipe";
        return result;
    }

    // The child writes to the pipe as its stdout; stderr is left attached to
    // ours so diagnostics stay visible and can never corrupt the payload.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
    posix_spawn_file_actions_addclose(&actions, pipeFds[1]);

    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable);
    for (const std::string& argument : arguments) storage.push_back(argument);

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, executable.c_str(), &actions, nullptr,
                                      argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipeFds[1]);

    if (spawned != 0) {
        ::close(pipeFds[0]);
        result.failureReason =
            "could not start the scanner: " + std::string(std::strerror(spawned));
        return result;
    }
    result.started = true;

    // Read until EOF or the deadline. Reading continuously rather than waiting
    // for the child first is what keeps a chatty plugin from filling the pipe
    // buffer and deadlocking both processes.
    const auto deadline = Clock::now() + timeout;
    std::array<char, 4096> buffer{};
    bool finishedReading = false;
    while (!finishedReading) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        if (remaining.count() <= 0) {
            result.timedOut = true;
            break;
        }

        pollfd descriptor{};
        descriptor.fd = pipeFds[0];
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, int(remaining.count()));
        if (ready < 0) {
            if (errno == EINTR) continue;
            result.failureReason = "poll failed while reading the scanner";
            break;
        }
        if (ready == 0) {
            result.timedOut = true;
            break;
        }

        const ssize_t count = ::read(pipeFds[0], buffer.data(), buffer.size());
        if (count > 0) {
            result.output.append(buffer.data(), std::size_t(count));
        } else if (count == 0) {
            finishedReading = true;   // the child closed its stdout
        } else if (errno != EINTR) {
            result.failureReason = "read failed while collecting scanner output";
            break;
        }
    }
    ::close(pipeFds[0]);

    if (result.timedOut) {
        // SIGKILL, not SIGTERM: a plugin that hung in a static constructor is
        // in no state to run a handler.
        ::kill(pid, SIGKILL);
    }

    // Reap unconditionally — an unwaited child is a zombie, and a long scan
    // would leave hundreds of them.
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    if (result.timedOut) {
        result.failureReason = "timed out after " + std::to_string(timeout.count()) + " ms";
        return result;
    }
    if (WIFSIGNALED(status)) {
        result.crashed = true;
        result.failureReason = "crashed (signal " + std::to_string(WTERMSIG(status)) + ")";
        return result;
    }
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
        if (result.exitCode != 0) {
            result.crashed = true;
            result.failureReason = describeExit(result.exitCode);
        }
    }
    return result;
}

#else

ScanProcessResult ScanProcess::run(const std::string& executable,
                                   const std::vector<std::string>& arguments,
                                   std::chrono::milliseconds timeout) {
    ScanProcessResult result;

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!::CreatePipe(&readEnd, &writeEnd, &security, 0)) {
        result.failureReason = "could not create a pipe";
        return result;
    }
    ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

    const std::wstring executablePath = platform::pathFromUtf8(executable).wstring();
    std::wstring commandLine = makeWindowsCommandLine(executablePath, arguments);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    const BOOL created =
        ::CreateProcessW(executablePath.c_str(), mutableCommandLine.data(),
                         nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                         nullptr, &startup, &process);
    ::CloseHandle(writeEnd);
    if (!created) {
        ::CloseHandle(readEnd);
        result.failureReason = "could not start the scanner";
        return result;
    }
    result.started = true;

    // A reader thread, for the same reason POSIX polls: a child that fills the
    // pipe buffer blocks until someone drains it, and waiting on the process
    // first would deadlock.
    std::string collected;
    std::thread reader([&] {
        char buffer[4096];
        DWORD count = 0;
        while (::ReadFile(readEnd, buffer, sizeof(buffer), &count, nullptr) && count > 0) {
            collected.append(buffer, count);
        }
    });

    const DWORD waited = ::WaitForSingleObject(process.hProcess, DWORD(timeout.count()));
    if (waited == WAIT_TIMEOUT) {
        result.timedOut = true;
        ::TerminateProcess(process.hProcess, 1);
        ::WaitForSingleObject(process.hProcess, INFINITE);
    }
    // Closing the read end unblocks the reader once the child is gone.
    reader.join();
    ::CloseHandle(readEnd);
    result.output = std::move(collected);

    DWORD exitCode = 0;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hProcess);
    ::CloseHandle(process.hThread);

    if (result.timedOut) {
        result.failureReason = "timed out after " + std::to_string(timeout.count()) + " ms";
        return result;
    }
    result.exitCode = int(exitCode);
    if (result.exitCode != 0) {
        result.crashed = true;
        result.failureReason = describeExit(result.exitCode);
    }
    return result;
}

#endif

} // namespace daw

// ── spawnDetached ──
//
// Split out from run() rather than folded into it: the two want opposite
// things. run() waits for the child and reads its output; this one must return
// while the child keeps living, and never reads anything at all.

namespace daw {

#if !defined(_WIN32)

std::int64_t ScanProcess::spawnDetached(const std::string& executable,
                                        const std::vector<std::string>& arguments,
                                        int* parentEndFd) {
    if (parentEndFd) *parentEndFd = -1;

    int pipeFds[2] = {-1, -1};
    if (::pipe(pipeFds) != 0) return 0;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // The child gets the read end on a fixed descriptor it knows to watch.
    posix_spawn_file_actions_adddup2(&actions, pipeFds[0], kParentPipeFd);
    posix_spawn_file_actions_addclose(&actions, pipeFds[1]);

    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back(executable);
    for (const std::string& argument : arguments) storage.push_back(argument);

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) argv.push_back(value.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, executable.c_str(), &actions, nullptr,
                                      argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipeFds[0]);

    if (spawned != 0) {
        ::close(pipeFds[1]);
        return 0;
    }
    // Held open for the life of the process. Closing it — including by dying —
    // is the signal.
    if (parentEndFd) *parentEndFd = pipeFds[1];
    else ::close(pipeFds[1]);
    return static_cast<std::int64_t>(pid);
}

#else

std::int64_t ScanProcess::spawnDetached(const std::string& executable,
                                        const std::vector<std::string>& arguments,
                                        int* parentEndFd) {
    if (parentEndFd) *parentEndFd = -1;

    const std::wstring executablePath = platform::pathFromUtf8(executable).wstring();
    std::wstring commandLine = makeWindowsCommandLine(executablePath, arguments);
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(executablePath.c_str(), mutableCommandLine.data(),
                          nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          nullptr, &startup, &process)) {
        return 0;
    }
    // Nothing here waits for the child; the handles are released at once and
    // the guard watches this process by pid instead of by pipe.
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return static_cast<std::int64_t>(process.dwProcessId);
}

#endif

} // namespace daw
