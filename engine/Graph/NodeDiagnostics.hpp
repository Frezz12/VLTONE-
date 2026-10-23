#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace daw::engine {

struct NodeTimingSnapshot {
    std::uint64_t calls = 0, totalNs = 0, budgetNs = 0;
    std::uint64_t lastNs = 0, peakNs = 0, overruns = 0;
    std::uint64_t activeSinceNs = 0, completedAtNs = 0;
};

// One renderer writes a node; control-thread readers never reset its counters.
// No ring draining, allocation, locks or vendor calls on the observation path.
class NodeDiagnostics {
public:
    NodeDiagnostics() : id(++s_nextId) {}
    const std::uint64_t id;
    void begin(std::uint64_t now) noexcept { m_active.store(now, std::memory_order_relaxed); }
    void finish(std::uint64_t now, std::uint64_t elapsed, std::uint64_t budget) noexcept {
        m_total.store(m_total.load(std::memory_order_relaxed) + elapsed, std::memory_order_relaxed);
        m_budget.store(m_budget.load(std::memory_order_relaxed) + budget, std::memory_order_relaxed);
        m_last.store(elapsed, std::memory_order_relaxed);
        m_peak.store(std::max(elapsed, m_peak.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        if (budget && elapsed > budget)
            m_overruns.store(m_overruns.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        m_calls.store(m_calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        m_completed.store(now, std::memory_order_relaxed);
        m_active.store(0, std::memory_order_release);
    }
    // Independently sampled cumulative values; interval consumers clamp deltas.
    NodeTimingSnapshot snapshot() const noexcept {
        return {m_calls.load(std::memory_order_relaxed), m_total.load(std::memory_order_relaxed),
            m_budget.load(std::memory_order_relaxed), m_last.load(std::memory_order_relaxed),
            m_peak.load(std::memory_order_relaxed), m_overruns.load(std::memory_order_relaxed),
            m_active.load(std::memory_order_acquire), m_completed.load(std::memory_order_relaxed)};
    }
private:
    inline static std::atomic<std::uint64_t> s_nextId{0};
    std::atomic<std::uint64_t> m_calls{0}, m_total{0}, m_budget{0}, m_last{0}, m_peak{0};
    std::atomic<std::uint64_t> m_overruns{0}, m_active{0}, m_completed{0};
};

} // namespace daw::engine
