#pragma once
#include "Common/LockFreeQueue.hpp"
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>

namespace daw::engine {
class PcmReadScope;
// File mappings remain the immutable backing store. Only this worker (or an
// explicit control-thread warm-up) reads them; realtime readers pin RAM pages.
class PcmReadCache {
public:
    static constexpr std::size_t kPageSamples = 4096;
    static constexpr std::size_t kWays = 8;
    static PcmReadCache& instance();
    static PcmReadCache* existing() noexcept { return s_instance.load(std::memory_order_acquire); }
    explicit PcmReadCache(std::size_t budgetBytes = 256u * 1024u * 1024u);
    ~PcmReadCache();
    std::uint64_t addSource(const float* data, std::size_t samples);
    void removeSource(std::uint64_t id);
    void hint(std::uint64_t source, std::size_t first) noexcept;
    void warm(std::uint64_t id, std::size_t first, std::size_t count);
    void invalidateRequests() noexcept { m_epoch.fetch_add(1, std::memory_order_acq_rel); }
    struct Counters { std::uint64_t misses, droppedRequests, staleRequests; std::size_t capacityBytes, lockedBytes; };
    Counters counters() const noexcept;
private:
    friend class PcmReadScope;
    struct alignas(64) Slot {
        static constexpr int kWriter = 1 << 30;
        std::atomic<int> readers{0}; // High bit: writer; low bits: transient or pinned readers
        std::atomic<std::uint64_t> source{0}, page{0}, used{0};
        std::atomic<std::uint64_t> prefetched{0};
    };
    struct Request { std::uint64_t source, page, epoch; };
    struct Source { const float* data; std::size_t samples; };
    std::size_t firstSlot(std::uint64_t source, std::uint64_t page) const noexcept;
    std::size_t pin(std::uint64_t source, std::uint64_t page) noexcept;
    bool request(std::uint64_t source, std::uint64_t page) noexcept;
    void fill(std::uint64_t id, std::uint64_t page, const Source& source);
    void worker();
    bool m_locked = false;
    std::size_t m_count;
    std::unique_ptr<Slot[]> m_slots;
    std::unique_ptr<float[]> m_pcm;
    LockFreeMPSCQueue<Request, 16384> m_requests;
    std::mutex m_sourcesMutex;
    std::unordered_map<std::uint64_t, Source> m_sources;
    std::uint64_t m_nextSource = 1;
    std::atomic<std::uint64_t> m_epoch{1}, m_clock{1}, m_misses{0}, m_dropped{0}, m_stale{0};
    std::atomic<bool> m_running{true};
    std::thread m_worker;
    static inline std::atomic<PcmReadCache*> s_instance{nullptr};
};

// Stack-owned, bounded cursors. Pinning a page once amortizes cache lookup
// over all sample/interpolation reads in a node. No TLS allocation/destruction.
class PcmReadScope {
public:
    explicit PcmReadScope(bool realtime) noexcept;
    ~PcmReadScope();
    static PcmReadScope* current() noexcept { return s_current; }
    std::span<const float> view(PcmReadCache& cache, std::uint64_t source,
                                std::size_t sample, std::size_t count) noexcept;
private:
    struct Pin { PcmReadCache* cache = nullptr; std::uint64_t source = 0, page = 0; std::size_t slot = 0; const float* data = nullptr; };
    void release(Pin& pin) noexcept;
    std::array<Pin, 8> m_pins{};
    unsigned m_last = 0, m_next = 0;
    PcmReadScope* m_previous;
    static inline thread_local PcmReadScope* s_current = nullptr;
    static inline const std::array<float, PcmReadCache::kPageSamples> s_silence{};
};
} // namespace daw::engine
