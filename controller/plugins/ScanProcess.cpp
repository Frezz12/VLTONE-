#include "plugins/ScanProcess.hpp"
#include "platform/PathUtils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace daw {
namespace {
using Clock = std::chrono::steady_clock;

void appendOutput(ScanProcessResult& result, const char* data, std::size_t size, bool diagnostic) {
    if (diagnostic) {
        result.diagnostics.append(data, size);
        if (result.diagnostics.size() > kScanDiagnosticLimit)
            result.diagnostics.erase(0, result.diagnostics.size() - kScanDiagnosticLimit);
    } else if (size > kScanResponseLimit - result.output.size()) {
        result.outputLimitExceeded = true;
        result.failureReason = "scanner response exceeds 64 MiB";
    } else {
        result.output.append(data, size);
    }
}

bool interrupted(ScanProcessResult& result, const ScanProcessOptions& options,
                 Clock::time_point deadline, std::chrono::milliseconds timeout) {
    if (options.cancellation.stop_requested()) {
        result.cancelled = true;
        result.failureReason = "cancelled";
        return true;
    }
    if (Clock::now() >= deadline) {
        result.timedOut = true;
        result.failureReason = "timed out after " + std::to_string(timeout.count()) + " ms";
        return true;
    }
    return result.transportError || result.outputLimitExceeded;
}

#if defined(_WIN32)
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    void reset(HANDLE next = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value);
        value = next;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length) != length) return {};
    return result;
}

std::wstring quoteWindowsArgument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring result(1, L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') { ++backslashes; continue; }
        result.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        result.push_back(character);
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring makeWindowsCommandLine(const std::wstring& executable,
                                    const std::vector<std::string>& arguments) {
    std::wstring result = quoteWindowsArgument(executable);
    for (const auto& argument : arguments) {
        result.push_back(L' ');
        result += quoteWindowsArgument(utf8ToWide(argument));
    }
    return result;
}

bool outputPipe(Handle& parent, Handle& child, SECURITY_ATTRIBUTES& security) {
    return ::CreatePipe(&parent.value, &child.value, &security, 65536) &&
           ::SetHandleInformation(parent.value, HANDLE_FLAG_INHERIT, 0);
}

// Synchronous, nonblocking byte writes require no writer thread to join after
// cancellation. The child still gets an ordinary, blocking stdin handle.
bool inputPipe(Handle& parent, Handle& child, SECURITY_ATTRIBUTES& security) {
    static std::atomic<unsigned long> sequence{0};
    const std::wstring name = L"\\\\.\\pipe\\vlt-scan-" + std::to_wstring(::GetCurrentProcessId()) +
        L"-" + std::to_wstring(++sequence);
    parent.value = ::CreateNamedPipeW(name.c_str(),
        PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 0, nullptr);
    if (!parent) return false;
    child.value = ::CreateFileW(name.c_str(), GENERIC_READ, 0, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!child) return false;
    return ::ConnectNamedPipe(parent.value, nullptr) || ::GetLastError() == ERROR_PIPE_CONNECTED;
}

// Bounded per pass even if a plugin continuously writes diagnostics.
bool drain(Handle& pipe, ScanProcessResult& result, bool diagnostic) {
    if (!pipe) return false;
    DWORD available = 0;
    if (!::PeekNamedPipe(pipe.value, nullptr, 0, nullptr, &available, nullptr)) {
        if (::GetLastError() != ERROR_BROKEN_PIPE) {
            result.transportError = true;
            result.failureReason = "could not read scanner pipe";
        }
        pipe.reset();
        return false;
    }
    if (!available) return false;
    std::array<char, 65536> buffer{};
    DWORD count = 0;
    if (!::ReadFile(pipe.value, buffer.data(),
        std::min<DWORD>(available, DWORD(buffer.size())), &count, nullptr)) {
        result.transportError = true;
        result.failureReason = "could not collect scanner output";
        return false;
    }
    appendOutput(result, buffer.data(), count, diagnostic);
    return count != 0;
}
#else
// Protect pipe()+fcntl() on macOS and crash guard launches with the same lock.
std::mutex spawnMutex;
struct Fd {
    int value = -1;
    ~Fd() { reset(); }
    void reset(int next = -1) { if (value >= 0) ::close(value); value = next; }
};

bool makePipe(Fd& read, Fd& write) {
    int fds[2];
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) != 0) return false;
#else
    if (::pipe(fds) != 0) return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#endif
    for (auto& fd : fds) {
        if (fd < 4) {
            const int next = ::fcntl(fd, F_DUPFD_CLOEXEC, 4);
            ::close(fd);
            fd = next;
        }
    }
    read.value = fds[0]; write.value = fds[1];
    return read.value >= 0 && write.value >= 0;
}

void isolateDescriptors(posix_spawn_file_actions_t& actions,
                         posix_spawnattr_t& attr, short flags, int firstClosed) {
#if defined(__APPLE__)
    (void)actions; (void)firstClosed;
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(__GLIBC__) && __GLIBC_PREREQ(2, 34)
    posix_spawn_file_actions_addclosefrom_np(&actions, firstClosed);
#else
    const long maximum = ::sysconf(_SC_OPEN_MAX);
    for (int fd = firstClosed; fd < maximum; ++fd)
        if (::fcntl(fd, F_GETFD) >= 0) posix_spawn_file_actions_addclose(&actions, fd);
#endif
    posix_spawnattr_setflags(&attr, flags);
}

std::vector<char*> makeArgv(std::vector<std::string>& storage) {
    std::vector<char*> argv;
    for (auto& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);
    return argv;
}

// Do not change the DAW's signal disposition when a child closes stdin early.
struct BlockSigpipe {
    sigset_t old{}, set{};
    bool wasPending = false;
    BlockSigpipe() {
        sigemptyset(&set); sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, &old);
        sigset_t pending; sigpending(&pending);
        wasPending = sigismember(&pending, SIGPIPE);
    }
    ~BlockSigpipe() {
        sigset_t pending; sigpending(&pending);
        if (!wasPending && sigismember(&pending, SIGPIPE)) {
            int signal; sigwait(&set, &signal);
        }
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
    }
};

bool drain(Fd& fd, ScanProcessResult& result, bool diagnostic) {
    if (fd.value < 0) return false;
    std::array<char, 65536> buffer{};
    const auto count = ::read(fd.value, buffer.data(), buffer.size());
    if (count > 0) {
        appendOutput(result, buffer.data(), std::size_t(count), diagnostic);
        return true;
    }
    if (count == 0) fd.reset();
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        result.transportError = true;
        result.failureReason = "could not collect scanner output";
    }
    return false;
}
#endif
} // namespace

ScanProcessResult ScanProcess::run(const std::string& executable,
    const std::vector<std::string>& arguments, std::chrono::milliseconds timeout,
    ScanProcessOptions options) {
    ScanProcessResult result;
    if (options.cancellation.stop_requested()) {
        result.cancelled = true;
        result.failureReason = "cancelled";
        return result;
    }
    if (options.input.size() > kScanRequestLimit) {
        result.transportError = true;
        result.failureReason = "scanner request exceeds 1 MiB";
        return result;
    }
    const auto deadline = Clock::now() + timeout;
#if defined(_WIN32)
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle input, childInput, output, childOutput, errors, childErrors;
    if (!inputPipe(input, childInput, security) ||
        !outputPipe(output, childOutput, security) || !outputPipe(errors, childErrors, security)) {
        result.failureReason = "could not create scanner pipes";
        return result;
    }
    Handle job;
    job.value = ::CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !::SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                         &limits, sizeof(limits))) {
        result.failureReason = "could not create scanner process group";
        return result;
    }
    SIZE_T bytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> attributes(bytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput.value;
    startup.StartupInfo.hStdOutput = childOutput.value;
    startup.StartupInfo.hStdError = childErrors.value;
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!::InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) {
        result.failureReason = "could not initialize scanner handle list";
        return result;
    }
    HANDLE inherited[] = {childInput.value, childOutput.value, childErrors.value};
    const bool handlesReady = ::UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr);
    const auto executablePath = platform::pathFromUtf8(executable).wstring();
    auto commandLine = makeWindowsCommandLine(executablePath, arguments);
    PROCESS_INFORMATION process{};
    const bool created = handlesReady && ::CreateProcessW(executablePath.c_str(),
        commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &startup.StartupInfo, &process);
    const DWORD launchError = ::GetLastError();
    ::DeleteProcThreadAttributeList(startup.lpAttributeList);
    childInput.reset(); childOutput.reset(); childErrors.reset();
    if (!created) {
        result.failureReason = "could not start the scanner (Windows error " +
                               std::to_string(launchError) + ")";
        return result;
    }
    Handle processHandle, threadHandle;
    processHandle.value = process.hProcess; threadHandle.value = process.hThread;
    if (!::AssignProcessToJobObject(job.value, process.hProcess) ||
        ::ResumeThread(process.hThread) == DWORD(-1)) {
        ::TerminateProcess(process.hProcess, 1);
        ::WaitForSingleObject(process.hProcess, 500);
        result.failureReason = "could not isolate scanner process";
        return result;
    }
    result.started = true;
    threadHandle.reset();
    std::size_t written = 0;
    bool exited = false;
    for (;;) {
        if (interrupted(result, options, deadline, timeout)) break;
        const bool outReady = drain(output, result, false);
        const bool errReady = drain(errors, result, true);
        if (input && written < options.input.size()) {
            DWORD count = 0;
            if (::WriteFile(input.value, options.input.data() + written,
                DWORD(std::min<std::size_t>(65536, options.input.size() - written)), &count, nullptr)) {
                written += count;
            } else if (::GetLastError() == ERROR_BROKEN_PIPE || ::GetLastError() == ERROR_NO_DATA) {
                input.reset();
            } else {
                result.transportError = true;
                result.failureReason = "could not write scanner request";
            }
        }
        if (written == options.input.size()) input.reset();
        if (::WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            exited = true;
            if (!outReady && !errReady) break;
        }
        if (!outReady && !errReady) ::Sleep(5);
    }
    // Descendants cannot retain a pipe beyond this job. Never wait for pipe EOF.
    ::TerminateJobObject(job.value, 1);
    if (!exited && ::WaitForSingleObject(process.hProcess, 500) != WAIT_OBJECT_0) {
        result.transportError = true;
        result.failureReason = "could not stop scanner process";
    }
    DWORD exitCode = DWORD(-1);
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);
#else
    Fd input, childInput, output, childOutput, errors, childErrors;
    std::unique_lock spawnLock(spawnMutex);
    if (!makePipe(childInput, input) || !makePipe(output, childOutput) ||
        !makePipe(errors, childErrors)) {
        result.failureReason = "could not create scanner pipes";
        return result;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, childInput.value, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, childOutput.value, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, childErrors.value, STDERR_FILENO);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0);
    isolateDescriptors(actions, attr, POSIX_SPAWN_SETPGROUP, 3);
    std::vector<std::string> storage{executable};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    auto argv = makeArgv(storage);
    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, executable.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    childInput.reset(); childOutput.reset(); childErrors.reset();
    spawnLock.unlock();
    if (spawned != 0) {
        result.failureReason = "could not start the scanner: " + std::string(std::strerror(spawned));
        return result;
    }
    result.started = true;
    for (const int fd : {input.value, output.value, errors.value})
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    BlockSigpipe blockSigpipe;
    std::size_t written = 0;
    bool exited = false, reaped = false;
    int status = 0;
    for (;;) {
        if (interrupted(result, options, deadline, timeout)) break;
        const bool outReady = drain(output, result, false);
        const bool errReady = drain(errors, result, true);
        if (input.value >= 0 && written < options.input.size()) {
            const auto count = ::write(input.value, options.input.data() + written,
                                      std::min<std::size_t>(65536, options.input.size() - written));
            if (count > 0) written += std::size_t(count);
            else if (count < 0 && errno == EPIPE) input.reset();
            else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                result.transportError = true;
                result.failureReason = "could not write scanner request";
            }
        }
        if (written == options.input.size()) input.reset();
        if (!exited) {
            // Keep the leader waitable until group cleanup. Reaping it early
            // would allow its pid/pgid to be reused before we kill descendants.
            siginfo_t information{};
            const int waited = ::waitid(P_PID, id_t(pid), &information, WEXITED | WNOHANG | WNOWAIT);
            exited = waited == 0 && information.si_pid == pid;
            if (waited < 0 && errno != EINTR) {
                result.transportError = true;
                result.failureReason = "could not wait for scanner";
                break;
            }
        }
        if (exited && !outReady && !errReady) break;
        if (!outReady && !errReady) {
            pollfd fds[] = {{output.value, POLLIN, 0}, {errors.value, POLLIN, 0},
                           {input.value, POLLOUT, 0}};
            ::poll(fds, 3, 5);
        }
    }
    ::kill(-pid, SIGKILL);
    if (!reaped) {
        const auto cleanupDeadline = Clock::now() + std::chrono::milliseconds(500);
        do {
            if (::waitpid(pid, &status, WNOHANG) == pid) { reaped = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (Clock::now() < cleanupDeadline);
        if (!reaped) {
            result.transportError = true;
            result.failureReason = "could not stop scanner process";
            // An OS-level uninterruptible wait cannot hold up cancellation.
            std::thread([pid] { int s; while (::waitpid(pid, &s, 0) < 0 && errno == EINTR) {} }).detach();
        }
    }
    if (reaped && WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    if (reaped && WIFSIGNALED(status) && !result.cancelled && !result.timedOut) {
        result.crashed = true;
        if (result.failureReason.empty())
            result.failureReason = "crashed (signal " + std::to_string(WTERMSIG(status)) + ")";
    }
#endif
    if (result.exitCode != 0 && !result.cancelled && !result.timedOut) {
        result.crashed = true;
        if (result.failureReason.empty())
            result.failureReason = "exited with code " + std::to_string(result.exitCode);
    }
    return result;
}

std::int64_t ScanProcess::spawnDetached(const std::string& executable,
    const std::vector<std::string>& arguments, int* parentEndFd) {
    if (parentEndFd) *parentEndFd = -1;
#if defined(_WIN32)
    const auto executablePath = platform::pathFromUtf8(executable).wstring();
    auto commandLine = makeWindowsCommandLine(executablePath, arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(executablePath.c_str(), commandLine.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 0;
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return static_cast<std::int64_t>(process.dwProcessId);
#else
    std::lock_guard lock(spawnMutex);
    Fd read, write;
    if (!makePipe(read, write)) return 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, read.value, kParentPipeFd);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    isolateDescriptors(actions, attr, 0, kParentPipeFd + 1);
    std::vector<std::string> storage{executable};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    auto argv = makeArgv(storage);
    pid_t pid = -1;
    const int spawned = ::posix_spawn(&pid, executable.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (spawned != 0) return 0;
    if (parentEndFd) { *parentEndFd = write.value; write.value = -1; }
    return static_cast<std::int64_t>(pid);
#endif
}
} // namespace daw
