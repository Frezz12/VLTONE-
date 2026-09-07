#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace daw::rt {

inline std::uint64_t nowNanos() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One producer, one diagnostics consumer. Storage and both indices are owned
// for the lifetime of the stream; a slow reader drops telemetry, never audio.
template <class T, std::size_t Capacity> class DiagnosticRing {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0);
public:
    bool push(const T& item) noexcept {
        const auto write = m_write.load(std::memory_order_relaxed);
        const auto next = (write + 1) & (Capacity - 1);
        if (next == m_read.load(std::memory_order_acquire)) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_items[write] = item;
        m_write.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& item) noexcept {
        const auto read = m_read.load(std::memory_order_relaxed);
        if (read == m_write.load(std::memory_order_acquire)) return false;
        item = m_items[read];
        m_read.store((read + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }
    std::uint64_t dropped() const noexcept { return m_dropped.load(std::memory_order_relaxed); }
private:
    std::array<T, Capacity> m_items{};
    alignas(64) std::atomic<std::size_t> m_read{0};
    alignas(64) std::atomic<std::size_t> m_write{0};
    std::atomic<std::uint64_t> m_dropped{0};
};

struct BlockTiming {
    std::uint64_t elapsedNs = 0;
    std::uint64_t budgetNs = 0;
    std::uint32_t frames = 0;
    std::uint32_t flags = 0;
};
struct TimingCounters {
    std::uint64_t blocks = 0, overruns = 0, maximumNs = 0, dropped = 0;
};
class BlockMetrics {
public:
    void record(std::uint64_t elapsedNs, std::uint32_t frames, double rate,
                std::uint32_t flags = 0) noexcept {
        const auto budget = rate > 0 ? std::uint64_t(double(frames) * 1e9 / rate) : 0;
        m_blocks.fetch_add(1, std::memory_order_relaxed);
        if (budget && elapsedNs > budget) m_overruns.fetch_add(1, std::memory_order_relaxed);
        // Single producer; readers never reset these lifetime counters.
        if (elapsedNs > m_maximum.load(std::memory_order_relaxed))
            m_maximum.store(elapsedNs, std::memory_order_relaxed);
        m_events.push({elapsedNs, budget, frames, flags});
    }
    bool pop(BlockTiming& timing) noexcept { return m_events.pop(timing); }
    TimingCounters counters() const noexcept {
        return {m_blocks.load(std::memory_order_relaxed),
                m_overruns.load(std::memory_order_relaxed),
                m_maximum.load(std::memory_order_relaxed), m_events.dropped()};
    }
private:
    DiagnosticRing<BlockTiming, 8192> m_events;
    std::atomic<std::uint64_t> m_blocks{0}, m_overruns{0}, m_maximum{0};
};

struct TimingSummary {
    std::size_t count = 0;
    double meanMs = 0, p95Ms = 0, p99Ms = 0, p999Ms = 0, maximumMs = 0;
    double p999Load = 0;
    std::uint64_t overruns = 0;
};
// Control/benchmark thread only. Quantiles are calculated from individual
// blocks, never from the transport's exponential DSP-load display.
class TimingAccumulator {
public:
    void drain(BlockMetrics& metrics) {
        BlockTiming event;
        // Bound a drain even if a producer is continuously running offline.
        for (unsigned i = 0; i < 8192 && metrics.pop(event); ++i) {
            m_times.push_back(double(event.elapsedNs) / 1e6);
            m_loads.push_back(event.budgetNs ? double(event.elapsedNs) / event.budgetNs : 0);
            if (event.budgetNs && event.elapsedNs > event.budgetNs) ++m_overruns;
        }
    }
    TimingSummary summary() const {
        TimingSummary result;
        result.count = m_times.size();
        result.overruns = m_overruns;
        if (m_times.empty()) return result;
        auto times = m_times;
        auto loads = m_loads;
        std::sort(times.begin(), times.end());
        std::sort(loads.begin(), loads.end());
        const auto at = [](const auto& values, double quantile) {
            return values[std::size_t(std::ceil(quantile * values.size())) - 1];
        };
        for (double time : times) result.meanMs += time / times.size();
        result.p95Ms = at(times, .95); result.p99Ms = at(times, .99);
        result.p999Ms = at(times, .999); result.maximumMs = times.back();
        result.p999Load = at(loads, .999);
        return result;
    }
private:
    std::vector<double> m_times, m_loads;
    std::uint64_t m_overruns = 0;
};

struct ProfileEvent {
    enum class Kind : std::uint8_t { Node, Wait };
    std::uint64_t generation = 0, elapsedNs = 0;
    std::int64_t position = 0;
    std::uint32_t node = 0, worker = 0;
    Kind kind = Kind::Node;
};
} // namespace daw::rt
