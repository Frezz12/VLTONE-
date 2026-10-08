#include "SharedProcess.hpp"
#include "PluginProcessProtocol.hpp"
#include "platform/PathUtils.hpp"

#include <charconv>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace daw::plugins::ipc {
namespace {
std::uint64_t argument(int argc, char** argv, std::string_view name) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view value(argv[i]);
        if (!value.starts_with(name)) continue;
        const auto number = value.substr(name.size());
        std::uint64_t result = 0;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), result);
        if (parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size()) return result;
    }
    return 0;
}
#ifdef _WIN32
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    void reset(HANDLE next = nullptr) {
        if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value);
        value = next;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
#else
struct Fd {
    int value = -1;
    ~Fd() { reset(); }
    void reset(int next = -1) { if (value >= 0) ::close(value); value = next; }
};
bool ownFd(Fd& out, int fd) {
    if (fd < 0) return false;
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 7);
    ::close(fd);
    out.reset(moved);
    return moved >= 0;
}
#endif
}

struct SharedProcess::Impl {
    std::byte* memory = nullptr;
    std::size_t bytes = 0;
    std::uint64_t id = 0;
    bool child = false;
#ifdef _WIN32
    Handle mapping, wake, audioWake, process, job;
#else
    Fd mapping, wake, childWake, audioWake, childAudioWake, life, childLife;
    Fd audioControlRead, audioControlWrite;
    pid_t pid = 0;
#endif
};

SharedProcess::SharedProcess() : m(std::make_unique<Impl>()) {}
SharedProcess::~SharedProcess() {
    stop();
#ifdef _WIN32
    if (m->memory) ::UnmapViewOfFile(m->memory);
#else
    if (m->memory) ::munmap(m->memory, m->bytes);
#endif
}
std::byte* SharedProcess::data() const noexcept { return m->memory; }
std::size_t SharedProcess::size() const noexcept { return m->bytes; }
std::uint64_t SharedProcess::processId() const noexcept { return m->id; }

bool SharedProcess::create(std::size_t bytes, std::string& error) {
    if (m->memory || bytes < sizeof(Header) || bytes > kMaxMappingBytes) {
        error = "invalid shared-memory capacity"; return false;
    }
    m->bytes = bytes;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, FALSE};
    m->mapping.reset(::CreateFileMappingW(INVALID_HANDLE_VALUE, &security,
        PAGE_READWRITE, 0, DWORD(bytes), nullptr));
    m->wake.reset(::CreateEventW(&security, FALSE, FALSE, nullptr));
    m->audioWake.reset(::CreateEventW(&security, FALSE, FALSE, nullptr));
    if (m->mapping && m->wake && m->audioWake)
        m->memory = static_cast<std::byte*>(::MapViewOfFile(m->mapping.value,
            FILE_MAP_ALL_ACCESS, 0, 0, bytes));
#else
    char path[] = "/tmp/vlt-plugin-XXXXXX";
    const int fd = ::mkstemp(path);
    if (fd >= 0) ::unlink(path); // private, anonymous after creation
    if (!ownFd(m->mapping, fd) || ::ftruncate(m->mapping.value, off_t(bytes)) != 0) {
        error = "could not create shared-memory backing"; return false;
    }
    void* memory = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, m->mapping.value, 0);
    if (memory != MAP_FAILED) m->memory = static_cast<std::byte*>(memory);
    int wakes[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, wakes) != 0) {
        error = "could not create wake socket"; return false;
    }
    const bool wakeA = ownFd(m->wake, wakes[0]);
    const bool wakeB = ownFd(m->childWake, wakes[1]);
    int audioWakes[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, audioWakes) != 0) {
        error = "could not create audio wake socket"; return false;
    }
    const bool audioA = ownFd(m->audioWake, audioWakes[0]);
    const bool audioB = ownFd(m->childAudioWake, audioWakes[1]);
    int lives[2];
    if (::pipe(lives) != 0) { error = "could not create lifetime pipe"; return false; }
    const bool lifeA = ownFd(m->childLife, lives[0]);
    const bool lifeB = ownFd(m->life, lives[1]);
    if (!wakeA || !wakeB || !audioA || !audioB || !lifeA || !lifeB) {
        error = "could not isolate inherited descriptors"; return false;
    }
    if (::fcntl(m->wake.value, F_SETFL, O_NONBLOCK) != 0 ||
        ::fcntl(m->audioWake.value, F_SETFL, O_NONBLOCK) != 0) {
        error = "could not configure nonblocking wake"; return false;
    }
#ifdef __APPLE__
    const int yes = 1;
    if (::setsockopt(m->wake.value, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0 ||
        ::setsockopt(m->audioWake.value, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) != 0) {
        error = "could not configure wake socket"; return false;
    }
#endif
#endif
    if (!m->memory) { error = "could not map plugin transport"; return false; }
    return true;
}

bool SharedProcess::launch(const std::string& executable, std::string& error) {
    if (!m->memory || m->id || m->child) { error = "invalid launch state"; return false; }
    if (executable.empty() || executable.find('\0') != std::string::npos) {
        error = "invalid host executable path"; return false;
    }
#ifdef _WIN32
    const auto path = platform::pathFromUtf8(executable).wstring();
    // A Windows executable path cannot contain a quote. Its final component
    // is a filename, so it cannot end in an unescaped directory separator.
    if (path.find(L'"') != std::wstring::npos || path.back() == L'\\') {
        error = "invalid host executable path"; return false;
    }
    std::wstring command = L"\"" + path + L"\" --vlt-mapping=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(m->mapping.value)) +
        L" --vlt-wake=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(m->wake.value)) +
        L" --vlt-audio-wake=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(m->audioWake.value)) +
        L" --vlt-bytes=" + std::to_wstring(m->bytes);
    m->job.reset(::CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!m->job || !::SetInformationJobObject(m->job.value, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) { error = "could not create host job"; return false; }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle null;
    null.reset(::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!null) { error = "could not isolate host standard streams"; return false; }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = null.value;
    SIZE_T attributeBytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 2, 0, &attributeBytes);
    std::vector<std::byte> attributes(attributeBytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!::InitializeProcThreadAttributeList(startup.lpAttributeList, 2, 0, &attributeBytes)) {
        error = "could not initialize host attributes"; return false;
    }
    HANDLE inherited[]{m->mapping.value, m->wake.value, m->audioWake.value, null.value};
    // Atomic job assignment avoids a suspended child escaping if its parent
    // dies between CreateProcess and AssignProcessToJobObject.
    const bool configured = ::SetHandleInformation(m->mapping.value, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) &&
        ::SetHandleInformation(m->wake.value, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) &&
        ::SetHandleInformation(m->audioWake.value, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) &&
        ::UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) &&
        ::UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
            &m->job.value, sizeof(HANDLE), nullptr, nullptr);
    PROCESS_INFORMATION process{};
    const bool created = configured && ::CreateProcessW(path.c_str(), command.data(), nullptr,
        nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
        &startup.StartupInfo, &process);
    const auto code = ::GetLastError();
    ::SetHandleInformation(m->mapping.value, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(m->wake.value, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(m->audioWake.value, HANDLE_FLAG_INHERIT, 0);
    ::DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!created) { error = "could not launch plugin host (Windows " + std::to_string(code) + ")"; return false; }
    ::CloseHandle(process.hThread);
    m->process.reset(process.hProcess);
    m->id = process.dwProcessId;
#else
    std::vector<std::string> args{executable, "--vlt-mapping=3", "--vlt-wake=4",
        "--vlt-life=5", "--vlt-audio-wake=6", "--vlt-bytes=" + std::to_string(m->bytes)};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&actions) != 0) { error = "spawn actions failed"; return false; }
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&actions); error = "spawn attributes failed"; return false;
    }
    int result = 0;
    const auto checked = [&](int value) { if (!result) result = value; };
    checked(posix_spawn_file_actions_adddup2(&actions, m->mapping.value, 3));
    checked(posix_spawn_file_actions_adddup2(&actions, m->childWake.value, 4));
    checked(posix_spawn_file_actions_adddup2(&actions, m->childLife.value, 5));
    checked(posix_spawn_file_actions_adddup2(&actions, m->childAudioWake.value, 6));
    for (int fd = 0; fd < 3; ++fd)
        checked(posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0));
    short flags = POSIX_SPAWN_SETPGROUP;
#ifdef __APPLE__
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#elif defined(__GLIBC__) && __GLIBC_PREREQ(2, 34)
    checked(posix_spawn_file_actions_addclosefrom_np(&actions, 7));
#else
    const long maximum = ::sysconf(_SC_OPEN_MAX);
    for (int fd = 7; fd < maximum; ++fd)
        if (::fcntl(fd, F_GETFD) >= 0) checked(posix_spawn_file_actions_addclose(&actions, fd));
#endif
    checked(posix_spawnattr_setflags(&attr, flags));
    checked(posix_spawnattr_setpgroup(&attr, 0));
    if (!result) result = posix_spawn(&m->pid, executable.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    m->childWake.reset(); m->childAudioWake.reset(); m->childLife.reset();
    if (result) { m->pid = 0; error = "could not launch plugin host (POSIX " + std::to_string(result) + ")"; return false; }
    m->id = std::uint64_t(m->pid);
#endif
    return true;
}

bool SharedProcess::attach(int argc, char** argv, std::string& error) {
    const auto bytes = argument(argc, argv, "--vlt-bytes=");
    const auto mapping = argument(argc, argv, "--vlt-mapping=");
    const auto wake = argument(argc, argv, "--vlt-wake=");
    const auto audioWake = argument(argc, argv, "--vlt-audio-wake=");
    if (bytes < sizeof(Header) || bytes > kMaxMappingBytes || !mapping || !wake || !audioWake) {
        error = "invalid transport arguments"; return false;
    }
    m->bytes = std::size_t(bytes); m->child = true;
#ifdef _WIN32
    m->mapping.reset(reinterpret_cast<HANDLE>(std::uintptr_t(mapping)));
    m->wake.reset(reinterpret_cast<HANDLE>(std::uintptr_t(wake)));
    m->audioWake.reset(reinterpret_cast<HANDLE>(std::uintptr_t(audioWake)));
    ::SetHandleInformation(m->mapping.value, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(m->wake.value, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(m->audioWake.value, HANDLE_FLAG_INHERIT, 0);
    m->memory = static_cast<std::byte*>(::MapViewOfFile(m->mapping.value,
        FILE_MAP_ALL_ACCESS, 0, 0, m->bytes));
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
    if (mapping != 3 || wake != 4 || audioWake != 6 || argument(argc, argv, "--vlt-life=") != 5) {
        error = "invalid inherited descriptors"; return false;
    }
    m->mapping.reset(3); m->wake.reset(4); m->childLife.reset(5); m->audioWake.reset(6);
    for (int fd = 3; fd <= 6; ++fd) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    int control[2];
    if (::pipe(control) != 0) { error = "could not create local audio wake"; return false; }
    const bool controlRead = ownFd(m->audioControlRead, control[0]);
    const bool controlWrite = ownFd(m->audioControlWrite, control[1]);
    if (!controlRead || !controlWrite || ::fcntl(m->audioControlWrite.value, F_SETFL, O_NONBLOCK) != 0) {
        error = "could not configure local audio wake"; return false;
    }
    struct stat info{};
    if (::fstat(3, &info) != 0 || info.st_size != off_t(bytes)) {
        error = "shared-memory size mismatch"; return false;
    }
    void* memory = ::mmap(nullptr, m->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, 3, 0);
    if (memory != MAP_FAILED) m->memory = static_cast<std::byte*>(memory);
    // Main/DSP threads may hang inside foreign code. An independent lifetime
    // watcher still observes the parent's pipe closing and ends this group.
    std::thread([] {
        char byte;
        ssize_t count;
        do { count = ::read(5, &byte, 1); } while (count < 0 && errno == EINTR);
        if (::getpgrp() == ::getpid()) ::kill(0, SIGKILL);
        std::_Exit(1);
    }).detach();
#endif
    if (!m->memory) { error = "could not attach plugin transport"; return false; }
    return true;
}

bool SharedProcess::signal(bool audio) noexcept {
    const auto wake = audio ? m->audioWake.value : m->wake.value;
#ifdef _WIN32
    return ::SetEvent(wake) != 0;
#else
    const char byte = 1;
#ifdef MSG_NOSIGNAL
    const auto count = ::send(wake, &byte, 1, MSG_NOSIGNAL);
#else
    const auto count = ::send(wake, &byte, 1, 0);
#endif
    return count == 1 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
#endif
}
bool SharedProcess::wait(int timeoutMs, bool audio) {
    const auto wake = audio ? m->audioWake.value : m->wake.value;
#ifdef _WIN32
    const auto result = ::WaitForSingleObject(wake, timeoutMs < 0 ? INFINITE : DWORD(timeoutMs));
    return result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT;
#else
    pollfd descriptors[2]{{wake, POLLIN, 0}, {m->audioControlRead.value, POLLIN, 0}};
    int ready;
    do { ready = ::poll(descriptors, audio ? 2 : 1, timeoutMs); } while (ready < 0 && errno == EINTR);
    if (!ready) return true;
    if (ready < 0) return false;
    char bytes[64];
    if (audio && (descriptors[1].revents & POLLIN)) {
        ssize_t count;
        do { count = ::read(m->audioControlRead.value, bytes, sizeof(bytes)); } while (count < 0 && errno == EINTR);
        return count > 0;
    }
    if (!(descriptors[0].revents & POLLIN)) return false;
    ssize_t count;
    do { count = ::recv(wake, bytes, sizeof(bytes), 0); } while (count < 0 && errno == EINTR);
    return count > 0;
#endif
}
bool SharedProcess::wakeAudioThread() noexcept {
#ifdef _WIN32
    return ::SetEvent(m->audioWake.value) != 0;
#else
    const char byte = 1;
    const auto count = ::write(m->audioControlWrite.value, &byte, 1);
    return count == 1 || (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
#endif
}
bool SharedProcess::running() {
#ifdef _WIN32
    return m->process && ::WaitForSingleObject(m->process.value, 0) == WAIT_TIMEOUT;
#else
    if (m->pid <= 0) return false;
    siginfo_t status{};
    // Leave the zombie owned until stop(), so its process-group ID cannot be
    // recycled before descendants have been killed.
    int result;
    do { result = ::waitid(P_PID, id_t(m->pid), &status, WEXITED | WNOHANG | WNOWAIT); }
    while (result < 0 && errno == EINTR);
    return result == 0 && status.si_pid == 0;
#endif
}
void SharedProcess::stop() {
    if (m->child) return;
#ifdef _WIN32
    if (m->job) ::TerminateJobObject(m->job.value, 1);
    if (m->process) ::WaitForSingleObject(m->process.value,
        DWORD(std::chrono::duration_cast<std::chrono::milliseconds>(kProcessStopWait).count()));
    m->process.reset(); m->job.reset();
#else
    if (m->pid > 0) {
        ::kill(-m->pid, SIGKILL);
        int status;
        while (::waitpid(m->pid, &status, 0) < 0 && errno == EINTR) {}
        m->pid = 0;
    }
    m->life.reset();
#endif
    m->id = 0;
}

} // namespace daw::plugins::ipc
