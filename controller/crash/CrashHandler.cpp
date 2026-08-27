#include "crash/CrashHandler.hpp"

#include "recovery/SessionFile.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <filesystem>
#include <iterator>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <dbghelp.h>
#else
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace daw::crash {

namespace {

/// Everything the handler touches is prepared here, before anything can go
/// wrong. Inside the handler nothing is allocated, formatted or locked.
int g_markerFd = -1;

constexpr std::size_t kPluginNameMax = 128;
char g_pluginName[kPluginNameMax] = {};
/// Reading a name the handler might catch mid-write is acceptable — a truncated
/// plugin name in a crash report is still a plugin name. What is not acceptable
/// is following a dangling pointer, which is why the name is copied here rather
/// than referenced.
std::atomic<bool> g_pluginActive{false};

std::atomic<bool> g_installed{false};
/// Guards against a fault inside the handler itself turning into an endless
/// loop of handlers.
std::atomic<bool> g_handling{false};

#if defined(_WIN32)
wchar_t g_markerPath[32768]{};
wchar_t g_dumpPath[32768]{};
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

void writeHandleText(HANDLE file, const char* text) {
    if (file == INVALID_HANDLE_VALUE || !text) return;
    std::size_t length = 0;
    while (text[length] != '\0') ++length;
    std::size_t writtenTotal = 0;
    while (writtenTotal < length) {
        DWORD written = 0;
        if (!WriteFile(file, text + writtenTotal,
                       DWORD(length - writtenTotal), &written, nullptr) || written == 0) return;
        writtenTotal += written;
    }
}

void writeHandleHex(HANDLE file, std::uintptr_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    char buffer[2 + sizeof(std::uintptr_t) * 2 + 1];
    int index = int(sizeof(buffer));
    buffer[--index] = '\0';
    if (value == 0) buffer[--index] = '0';
    while (value > 0 && index > 2) {
        buffer[--index] = digits[value & 0x0f];
        value >>= 4;
    }
    buffer[--index] = 'x';
    buffer[--index] = '0';
    writeHandleText(file, buffer + index);
}

LONG WINAPI handleUnhandledException(EXCEPTION_POINTERS* exception) {
    bool expected = false;
    if (!g_handling.compare_exchange_strong(expected, true))
        return EXCEPTION_CONTINUE_SEARCH;

    HANDLE marker = CreateFileW(g_markerPath, GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker != INVALID_HANDLE_VALUE) {
        writeHandleText(marker, "version=2\nexception=unhandled_windows_exception\npid=");
        writeHandleHex(marker, GetCurrentProcessId());
        writeHandleText(marker, "\nthread_id=");
        writeHandleHex(marker, GetCurrentThreadId());
        if (exception && exception->ExceptionRecord) {
            writeHandleText(marker, "\nexception_code=");
            writeHandleHex(marker, exception->ExceptionRecord->ExceptionCode);
            writeHandleText(marker, "\naddress=");
            writeHandleHex(marker, reinterpret_cast<std::uintptr_t>(
                exception->ExceptionRecord->ExceptionAddress));
        }
        if (g_pluginName[0] != '\0') {
            writeHandleText(marker, g_pluginActive.load(std::memory_order_relaxed)
                ? "\nplugin=" : "\nlast_plugin=");
            writeHandleText(marker, g_pluginName);
        }
        writeHandleText(marker, "\nminidump=crash.dmp\n");
        FlushFileBuffers(marker);
        CloseHandle(marker);
    }

    HANDLE dump = CreateFileW(g_dumpPath, GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION details{};
        details.ThreadId = GetCurrentThreadId();
        details.ExceptionPointers = exception;
        details.ClientPointers = FALSE;
        const auto type = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
            MiniDumpWithFullMemoryInfo | MiniDumpWithProcessThreadData |
            MiniDumpIgnoreInaccessibleMemory);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                          type, &details, nullptr, nullptr);
        FlushFileBuffers(dump);
        CloseHandle(dump);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#if !defined(_WIN32)

constexpr int kFatalSignals[] = {
    SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP,
#if defined(SIGSYS)
    SIGSYS,
#endif
};

/// A stack overflow exhausts the stack, so the handler cannot run on it. This
/// is the alternate one it runs on instead.
char* g_altStack = nullptr;

const char* signalName(int number) {
    switch (number) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        case SIGTRAP: return "SIGTRAP";
#if defined(SIGSYS)
        case SIGSYS: return "SIGSYS";
#endif
        default: break;
    }
    return "signal";
}

/// write(2) is async-signal-safe; everything in the standard library that would
/// make this convenient is not.
void emit(const char* text) {
    if (g_markerFd < 0 || !text) return;
    const std::size_t length = std::strlen(text);
    std::size_t written = 0;
    while (written < length) {
        const ssize_t n = ::write(g_markerFd, text + written, length - written);
        if (n <= 0) return;
        written += static_cast<std::size_t>(n);
    }
}

/// snprintf is not async-signal-safe, so the one number this file prints is
/// converted by hand.
void emitNumber(std::int64_t value) {
    char buffer[32];
    int index = int(sizeof(buffer));
    buffer[--index] = '\0';
    if (value == 0) buffer[--index] = '0';
    const bool negative = value < 0;
    std::uint64_t magnitude = negative
        ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
        : static_cast<std::uint64_t>(value);
    while (magnitude > 0 && index > 1) {
        buffer[--index] = char('0' + (magnitude % 10));
        magnitude /= 10;
    }
    if (negative && index > 0) buffer[--index] = '-';
    emit(buffer + index);
}

void emitHex(std::uintptr_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    char buffer[2 + sizeof(std::uintptr_t) * 2 + 1];
    int index = int(sizeof(buffer));
    buffer[--index] = '\0';
    if (value == 0) buffer[--index] = '0';
    while (value > 0 && index > 2) {
        buffer[--index] = digits[value & 0x0f];
        value >>= 4;
    }
    buffer[--index] = 'x';
    buffer[--index] = '0';
    emit(buffer + index);
}

extern "C" void handleFatalSignal(int number, siginfo_t* info, void* context) {
    (void)context;
    // A second fault while reporting the first must not loop forever.
    bool expected = false;
    if (!g_handling.compare_exchange_strong(expected, true)) ::_exit(134);

    emit("version=2\n");
    emit("pid=");
    emitNumber(static_cast<std::int64_t>(::getpid()));
    emit("\n");
    emit("signal=");
    emit(signalName(number));
    emit(" (");
    emitNumber(number);
    emit(")\n");

    if (info) {
        emit("signal_code=");
        emitNumber(info->si_code);
        emit("\n");
        emit("address=");
        emitHex(reinterpret_cast<std::uintptr_t>(info->si_addr));
        emit("\n");
    }

    if (g_pluginActive.load(std::memory_order_relaxed)) {
        emit("plugin=");
        emit(g_pluginName);
        emit("\n");
    } else if (g_pluginName[0] != '\0') {
        emit("last_plugin=");
        emit(g_pluginName);
        emit("\n");
    }

    // backtrace_symbols_fd writes straight to the descriptor. Its sibling
    // backtrace_symbols() allocates, which is exactly what must not happen
    // here.
    emit("stack:\n");
    void* frames[64];
    const int count = ::backtrace(frames, 64);
    if (count > 0 && g_markerFd >= 0) {
        ::backtrace_symbols_fd(frames, count, g_markerFd);
    }
    if (g_markerFd >= 0) ::fsync(g_markerFd);

    // Hand the signal back to the system: it produces its own crash report, and
    // the exit status ends up honestly reflecting the fault, which is what the
    // watchdog and the test both read.
    struct sigaction restore{};
    restore.sa_handler = SIG_DFL;
    sigemptyset(&restore.sa_mask);
    ::sigaction(number, &restore, nullptr);
    ::raise(number);
}

#endif // !_WIN32

} // namespace

bool install(const std::string& markerPath) {
    if (g_installed.load()) return true;
    // A handler that swallows faults makes a debugger useless, so there is a
    // way out that does not involve recompiling.
    if (std::getenv("DAW_NO_CRASH_HANDLER")) return false;

#if defined(_WIN32)
    const std::filesystem::path marker = std::filesystem::u8path(markerPath);
    const std::filesystem::path dump = marker.parent_path() / L"crash.dmp";
    if (marker.native().size() >= std::size(g_markerPath) ||
        dump.native().size() >= std::size(g_dumpPath)) return false;
    std::wmemcpy(g_markerPath, marker.c_str(), marker.native().size() + 1);
    std::wmemcpy(g_dumpPath, dump.c_str(), dump.native().size() + 1);
    g_previousFilter = SetUnhandledExceptionFilter(handleUnhandledException);
    g_installed.store(true);
    return true;
#else
    g_markerFd = ::open(markerPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (g_markerFd < 0) return false;

    static constexpr std::size_t kAltStackSize = SIGSTKSZ < 65536
                                                     ? 65536
                                                     : std::size_t(SIGSTKSZ);
    g_altStack = new char[kAltStackSize];
    stack_t alternate{};
    alternate.ss_sp = g_altStack;
    alternate.ss_size = kAltStackSize;
    alternate.ss_flags = 0;
    ::sigaltstack(&alternate, nullptr);

    struct sigaction action{};
    action.sa_sigaction = handleFatalSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int number : kFatalSignals) ::sigaction(number, &action, nullptr);

    g_installed.store(true);
    return true;
#endif
}

void uninstall() {
    if (!g_installed.load()) return;
#if defined(_WIN32)
    SetUnhandledExceptionFilter(g_previousFilter);
    g_previousFilter = nullptr;
#else
    struct sigaction restore{};
    restore.sa_handler = SIG_DFL;
    sigemptyset(&restore.sa_mask);
    for (int number : kFatalSignals) ::sigaction(number, &restore, nullptr);
    if (g_markerFd >= 0) ::close(g_markerFd);
    g_markerFd = -1;
    delete[] g_altStack;
    g_altStack = nullptr;
#endif
    g_installed.store(false);
}

void setPluginInFlight(const std::string& name) {
    const std::size_t length = std::min(name.size(), kPluginNameMax - 1);
    std::memcpy(g_pluginName, name.data(), length);
    g_pluginName[length] = '\0';
    g_pluginActive.store(true, std::memory_order_release);
}

std::string lastPluginName() { return g_pluginName; }

void clearPluginInFlight() {
    g_pluginActive.store(false, std::memory_order_release);
}

std::string readMarker(const std::string& markerPath) {
    return recovery::parseCrashMarker(markerPath);
}

} // namespace daw::crash
