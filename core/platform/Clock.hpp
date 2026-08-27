#pragma once

#include <chrono>
#include <cstdint>

// Portable monotonic clock. Replaces the macOS mach_absolute_time /
// mach_timebase_info pair. std::chrono::steady_clock is monotonic and, on
// Windows (QueryPerformanceCounter), macOS and Linux, reads without a syscall
// or lock — so it is safe to call from the realtime audio callback.
namespace audio::platform {

inline uint64_t nowNanos() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace audio::platform
