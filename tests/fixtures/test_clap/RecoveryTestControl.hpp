#pragma once
#include <array>
#include <atomic>

// Shared only by the fixture and its test. No production fault-injection API.
struct RecoveryTestControl {
    std::atomic<unsigned> next{0}, mode{0}, target{0};
    std::atomic<bool> failCreate{false}, failLoad{false}, failActivate{false}, failSave{false};
    std::atomic<unsigned> forbidden{0}, saves{0}, loads{0};
    std::array<std::atomic<unsigned>, 256> calls{};
};
