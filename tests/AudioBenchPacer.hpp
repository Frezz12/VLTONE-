#pragma once

#include <algorithm>
#include <chrono>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

// sleep_until can round an 8-sample period to 15.6 ms on Windows. Use the
// same high-resolution timer as the app's synthetic audio checks, without
// spinning or changing the system-wide timer resolution.
class AudioBenchPacer {
public:
#ifdef _WIN32
    AudioBenchPacer() : timer(CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE)) {}
    ~AudioBenchPacer() { if (timer) CloseHandle(timer); }
    AudioBenchPacer(const AudioBenchPacer&) = delete;
    AudioBenchPacer& operator=(const AudioBenchPacer&) = delete;
#endif
    void wait(std::chrono::steady_clock::time_point deadline) {
#ifdef _WIN32
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return;
        LARGE_INTEGER due;
        due.QuadPart = -std::max<LONGLONG>(1, remaining / 100);
        if (timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) &&
            WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) return;
#endif
        std::this_thread::sleep_until(deadline);
    }
private:
#ifdef _WIN32
    HANDLE timer = nullptr;
#endif
};
