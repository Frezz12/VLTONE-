#pragma once

#include <atomic>
#include <cmath>

namespace daw::engine {

/// Audio-side maximum, shared by every view of a channel. No sample traversal,
/// allocation or lock; a UI reset can race with the next block safely.
class PeakHold {
public:
    void observe(float peak) noexcept {
        if (!std::isfinite(peak)) return;
        float previous = m_peak.load(std::memory_order_relaxed);
        while (peak > previous && !m_peak.compare_exchange_weak(
                   previous, peak, std::memory_order_relaxed)) {}
    }
    float value() const noexcept { return m_peak.load(std::memory_order_relaxed); }
    void reset() noexcept { m_peak.store(0.0f, std::memory_order_relaxed); }

private:
    static_assert(std::atomic<float>::is_always_lock_free);
    std::atomic<float> m_peak{0.0f};
};

} // namespace daw::engine
