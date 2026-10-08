#pragma once
#include <cstdint>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace daw::test {
inline bool killChild(std::uint64_t pid) {
    if (!pid) return false;
#ifdef _WIN32
    const auto child = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, DWORD(pid));
    if (!child) return false;
    const bool stopped = TerminateProcess(child, 19) && WaitForSingleObject(child, 3000) == WAIT_OBJECT_0;
    CloseHandle(child);
    return stopped;
#else
    return kill(pid_t(pid), SIGKILL) == 0;
#endif
}
}
