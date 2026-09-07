#include "Memory/PcmReadCache.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#if !defined(_WIN32)
#include <sys/mman.h>
#endif
#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace daw::engine {
PcmReadCache& PcmReadCache::instance() {
    static PcmReadCache cache;
    s_instance.store(&cache, std::memory_order_release);
    return cache;
}
PcmReadCache::PcmReadCache(std::size_t budgetBytes) {
    const auto pages = std::max(kWays, budgetBytes / (kPageSamples * sizeof(float)));
    // Power-of-two set count makes the RT lookup a mask.
    m_count = kWays;
    while (m_count <= pages / 2) m_count *= 2;
    m_slots = std::make_unique<Slot[]>(m_count);
    m_pcm = std::make_unique<float[]>(m_count * kPageSamples);
    // Lock when permitted so memory pressure cannot swap the prepared PCM
    // itself. A refused lock is exposed in diagnostics, not treated as proof
    // of residency. No process-wide working-set limits are changed.
#if defined(_WIN32)
    m_locked = VirtualLock(m_pcm.get(), m_count * kPageSamples * sizeof(float)) != 0;
#else
    m_locked = mlock(m_pcm.get(), m_count * kPageSamples * sizeof(float)) == 0;
#endif
    // Value-initialization touches the pool before any audio reader sees it.
    m_worker = std::thread([this] { worker(); });
}
PcmReadCache::~PcmReadCache() {
    auto* self = this; s_instance.compare_exchange_strong(self, nullptr);
    m_running.store(false); m_worker.join();
    if (m_locked) {
#if defined(_WIN32)
        VirtualUnlock(m_pcm.get(), m_count * kPageSamples * sizeof(float));
#else
        munlock(m_pcm.get(), m_count * kPageSamples * sizeof(float));
#endif
    }
}
std::uint64_t PcmReadCache::addSource(const float* data, std::size_t samples) {
    std::lock_guard lock(m_sourcesMutex);
    const auto id = m_nextSource++;
    m_sources.emplace(id, Source{data, samples});
    return id;
}
void PcmReadCache::removeSource(std::uint64_t id) {
    // A background copy holds this mutex, so the mapping cannot disappear
    // underneath it. Queued requests contain IDs only and become harmless.
    std::lock_guard lock(m_sourcesMutex);
    m_sources.erase(id);
}
std::size_t PcmReadCache::firstSlot(std::uint64_t source, std::uint64_t page) const noexcept {
    std::uint64_t hash = source * 0x9e3779b97f4a7c15ull + page;
    hash ^= hash >> 30; hash *= 0xbf58476d1ce4e5b9ull; hash ^= hash >> 27;
    return std::size_t(hash & (m_count / kWays - 1)) * kWays;
}
std::size_t PcmReadCache::pin(std::uint64_t source, std::uint64_t page) noexcept {
    const auto first = firstSlot(source, page);
    for (std::size_t i = first; i < first + kWays; ++i) {
        auto& slot = m_slots[i];
        if (slot.source.load(std::memory_order_relaxed) != source ||
            slot.page.load(std::memory_order_relaxed) != page) continue;
        // Concurrent readers must never turn a ready page into silence merely
        // by losing a CAS race. The writer bit survives transient reader
        // increments; its release subtracts only that bit, preserving them.
        const int readers = slot.readers.fetch_add(1, std::memory_order_acquire);
        if (readers >= Slot::kWriter) {
            slot.readers.fetch_sub(1, std::memory_order_release);
            continue;
        }
        if (slot.source.load(std::memory_order_relaxed) == source && slot.page.load(std::memory_order_relaxed) == page) {
            slot.used.store(m_clock.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return i;
        }
        slot.readers.fetch_sub(1, std::memory_order_release);
    }
    return m_count;
}
bool PcmReadCache::request(std::uint64_t source, std::uint64_t page) noexcept {
    if (m_requests.push({source, page, m_epoch.load(std::memory_order_acquire)})) return true;
    m_dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}
void PcmReadCache::hint(std::uint64_t source, std::size_t first) noexcept {
    const auto page = first / kPageSamples;
    const auto slot = pin(source, page);
    if (slot == m_count) { request(source, page); return; }
    const auto stamp = (m_epoch.load(std::memory_order_relaxed) << 32) |
        (m_clock.load(std::memory_order_relaxed) / 32);
    if (m_slots[slot].prefetched.exchange(stamp, std::memory_order_relaxed) != stamp &&
        !request(source, page)) m_slots[slot].prefetched.store(0, std::memory_order_relaxed);
    m_slots[slot].readers.fetch_sub(1, std::memory_order_release);
}
void PcmReadCache::fill(std::uint64_t id, std::uint64_t page, const Source& source) {
    if (page >= (source.samples + kPageSamples - 1) / kPageSamples) return;
    const auto existing = pin(id, page);
    if (existing != m_count) { m_slots[existing].readers.fetch_sub(1, std::memory_order_release); return; }
    const auto first = firstSlot(id, page);
    std::size_t selected = m_count;
    auto oldest = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t i = first; i < first + kWays; ++i) {
        if (m_slots[i].readers.load(std::memory_order_relaxed) != 0) continue;
        const auto used = m_slots[i].used.load(std::memory_order_relaxed);
        if (used < oldest) { selected = i; oldest = used; }
    }
    if (selected == m_count) return;
    auto& slot = m_slots[selected];
    int unlocked = 0;
    if (!slot.readers.compare_exchange_strong(unlocked, Slot::kWriter, std::memory_order_acquire)) return;
    const auto start = std::size_t(page) * kPageSamples;
    const auto count = std::min(kPageSamples, source.samples - start);
    float* destination = m_pcm.get() + selected * kPageSamples;
    std::copy_n(source.data + start, count, destination);
    std::fill(destination + count, destination + kPageSamples, 0.f);
    slot.source.store(id, std::memory_order_relaxed); slot.page.store(page, std::memory_order_relaxed);
    slot.used.store(m_clock.load(std::memory_order_relaxed), std::memory_order_relaxed);
    slot.prefetched.store(false, std::memory_order_relaxed);
    slot.readers.fetch_sub(Slot::kWriter, std::memory_order_release);
}
void PcmReadCache::warm(std::uint64_t id, std::size_t first, std::size_t count) {
    std::lock_guard lock(m_sourcesMutex);
    const auto source = m_sources.find(id);
    if (source == m_sources.end() || first >= source->second.samples || count == 0) return;
    const auto end = first + std::min(count, source->second.samples - first);
    for (auto page = first / kPageSamples; page <= (end - 1) / kPageSamples; ++page) fill(id, page, source->second);
}
void PcmReadCache::worker() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    while (m_running.load(std::memory_order_relaxed)) {
        Request request;
        bool worked = false;
        m_clock.fetch_add(1, std::memory_order_relaxed);
        for (unsigned n = 0; n < 256 && m_requests.pop(request); ++n) {
            worked = true;
            if (request.epoch != m_epoch.load(std::memory_order_acquire)) { m_stale.fetch_add(1); continue; }
            std::lock_guard lock(m_sourcesMutex);
            const auto source = m_sources.find(request.source);
            if (source == m_sources.end()) continue;
            for (unsigned ahead = 0; ahead < 4; ++ahead) {
                if (request.epoch != m_epoch.load(std::memory_order_acquire)) break;
                fill(request.source, request.page + ahead, source->second);
            }
        }
        if (!worked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
PcmReadCache::Counters PcmReadCache::counters() const noexcept {
    return {m_misses.load(), m_dropped.load(), m_stale.load(), m_count * kPageSamples * sizeof(float),
        m_locked ? m_count * kPageSamples * sizeof(float) : 0};
}
PcmReadScope::PcmReadScope(bool realtime) noexcept : m_previous(s_current) { s_current = realtime ? this : nullptr; }
PcmReadScope::~PcmReadScope() { for (auto& pin : m_pins) release(pin); s_current = m_previous; }
void PcmReadScope::release(Pin& pin) noexcept {
    if (pin.cache && pin.slot < pin.cache->m_count) pin.cache->m_slots[pin.slot].readers.fetch_sub(1, std::memory_order_release);
    pin = {};
}
std::span<const float> PcmReadScope::view(PcmReadCache& cache, std::uint64_t source,
                                       std::size_t sample, std::size_t count) noexcept {
    const auto page = sample / PcmReadCache::kPageSamples;
    auto matches = [&](const Pin& pin) { return pin.cache == &cache && pin.source == source && pin.page == page; };
    if (!matches(m_pins[m_last])) {
        unsigned found = unsigned(m_pins.size());
        for (unsigned i = 0; i < m_pins.size(); ++i) if (matches(m_pins[i])) { found = i; break; }
        if (found == m_pins.size()) {
            found = m_next++ % unsigned(m_pins.size());
            auto& pin = m_pins[found]; release(pin);
            const auto slot = cache.pin(source, page);
            pin = {&cache, source, page, slot, slot < cache.m_count ? cache.m_pcm.get() + slot * PcmReadCache::kPageSamples : s_silence.data()};
            if (slot == cache.m_count) { cache.m_misses.fetch_add(1, std::memory_order_relaxed); cache.request(source, page); }
            else {
                const auto stamp = (cache.m_epoch.load(std::memory_order_relaxed) << 32) |
                    (cache.m_clock.load(std::memory_order_relaxed) / 32);
                if (cache.m_slots[slot].prefetched.exchange(stamp, std::memory_order_relaxed) != stamp &&
                    !cache.request(source, page))
                    cache.m_slots[slot].prefetched.store(0, std::memory_order_relaxed);
            }
        }
        m_last = found;
    }
    const auto offset = sample % PcmReadCache::kPageSamples;
    return {m_pins[m_last].data + offset, std::min(count, PcmReadCache::kPageSamples - offset)};
}
} // namespace daw::engine
