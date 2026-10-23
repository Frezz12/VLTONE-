#include "Memory/PcmReadCache.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <new>
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
    // Keep the full cache budget, but commit PCM only when a background reader
    // fills a page. Opening one short sample must not touch 256 MiB of silence.
    const auto bytes = m_count * kPageSamples * sizeof(float);
#if defined(_WIN32)
    m_pcm = static_cast<float*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE));
#else
    const auto address = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address != MAP_FAILED) m_pcm = static_cast<float*>(address);
#endif
    if (!m_pcm) throw std::bad_alloc();
    try {
        m_worker = std::thread([this] { worker(); });
    } catch (...) {
#if defined(_WIN32)
        VirtualFree(m_pcm, 0, MEM_RELEASE);
#else
        munmap(m_pcm, bytes);
#endif
        throw;
    }
}
PcmReadCache::~PcmReadCache() {
    auto* self = this; s_instance.compare_exchange_strong(self, nullptr);
    m_running.store(false); m_worker.join();
    // Releasing the mapping also releases any remaining page locks.
#if defined(_WIN32)
    VirtualFree(m_pcm, 0, MEM_RELEASE);
#else
    munmap(m_pcm, m_count * kPageSamples * sizeof(float));
#endif
}
void PcmReadCache::releasePage(Slot& slot) noexcept {
    if (!slot.allocated) return;
    auto* data = m_pcm + std::size_t(&slot - m_slots.get()) * kPageSamples;
    if (slot.locked) {
#if defined(_WIN32)
        VirtualUnlock(data, kPageSamples * sizeof(float));
#else
        munlock(data, kPageSamples * sizeof(float));
#endif
        slot.locked = false;
        m_lockedBytes.fetch_sub(kPageSamples * sizeof(float), std::memory_order_relaxed);
    }
#if defined(_WIN32)
    VirtualFree(data, kPageSamples * sizeof(float), MEM_DECOMMIT);
#else
    madvise(data, kPageSamples * sizeof(float), MADV_DONTNEED);
#endif
    slot.allocated = false;
    m_allocatedBytes.fetch_sub(kPageSamples * sizeof(float), std::memory_order_relaxed);
}
bool PcmReadCache::releaseSourcePages(std::uint64_t id) noexcept {
    bool complete = true;
    for (std::size_t i = 0; i < m_count; ++i) {
        auto& slot = m_slots[i];
        if (slot.source.load(std::memory_order_relaxed) != id) continue;
        int unlocked = 0;
        if (!slot.readers.compare_exchange_strong(unlocked, Slot::kWriter, std::memory_order_acquire)) {
            complete = false;
            continue;
        }
        slot.source.store(0, std::memory_order_relaxed);
        slot.used.store(0, std::memory_order_relaxed);
        releasePage(slot);
        slot.readers.fetch_sub(Slot::kWriter, std::memory_order_release);
    }
    return complete;
}
std::uint64_t PcmReadCache::addSource(std::shared_ptr<const float> data, std::size_t samples) {
    std::lock_guard lock(m_sourcesMutex);
    const auto id = m_nextSource++;
    m_sources.emplace(id, Source{std::move(data), samples});
    return id;
}
void PcmReadCache::removeSource(std::uint64_t id) {
    decltype(m_sources)::node_type retired;
    {
        std::lock_guard lock(m_sourcesMutex);
        retired = m_sources.extract(id);
    }
    // In-flight copies own the mapping; queued IDs simply stop resolving.
    // Even a last-reference unmap/destructor runs outside the metadata lock.
    {
        const std::lock_guard lock(m_fillMutex);
        if (!releaseSourcePages(id)) {
            m_retiredSources.push_back(id);
            m_retirementsPending.store(true, std::memory_order_release);
        }
    }
}
PcmReadCache::Source PcmReadCache::findSource(std::uint64_t id) {
    std::lock_guard lock(m_sourcesMutex);
    const auto found = m_sources.find(id);
    return found == m_sources.end() ? Source{} : found->second;
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
            slot.used.store(m_useClock.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return i;
        }
        slot.readers.fetch_sub(1, std::memory_order_release);
    }
    return m_count;
}
std::uint64_t PcmReadCache::requestKey(const Request& request) noexcept {
    auto key = request.source * 0x9e3779b97f4a7c15ull ^
        request.page * 0xbf58476d1ce4e5b9ull ^ request.epoch * 0x94d049bb133111ebull;
    key ^= key >> 30; key *= 0xbf58476d1ce4e5b9ull; key ^= key >> 27;
    return key ? key : 1;
}
void PcmReadCache::finishRequest(const Request& request) noexcept {
    auto key = requestKey(request);
    m_pendingRequests[key % m_pendingRequests.size()].compare_exchange_strong(
        key, 0, std::memory_order_relaxed);
}
bool PcmReadCache::request(std::uint64_t source, std::uint64_t page) noexcept {
    const Request request{source, page, m_epoch.load(std::memory_order_acquire)};
    const auto key = requestKey(request);
    // Many voices can miss the same page together. One queued request suffices;
    // collisions merely admit a duplicate, never suppress a different page.
    if (m_pendingRequests[key % m_pendingRequests.size()].exchange(key, std::memory_order_relaxed) == key)
        return true;
    if (m_requests.push(request)) return true;
    finishRequest(request);
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
    float* destination = m_pcm + selected * kPageSamples;
    const bool allocate = !slot.allocated;
    if (allocate) {
#if defined(_WIN32)
        if (!VirtualAlloc(destination, kPageSamples * sizeof(float), MEM_COMMIT, PAGE_READWRITE)) {
            slot.readers.fetch_sub(Slot::kWriter, std::memory_order_release);
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
#endif
        slot.allocated = true;
        m_allocatedBytes.fetch_add(kPageSamples * sizeof(float), std::memory_order_relaxed);
    }
    std::copy_n(source.data.get() + start, count, destination);
    std::fill(destination + count, destination + kPageSamples, 0.f);
    if (allocate) {
        // Touch and optionally lock the entire page before RT publication.
#if defined(_WIN32)
        slot.locked = VirtualLock(destination, kPageSamples * sizeof(float)) != 0;
#else
        slot.locked = mlock(destination, kPageSamples * sizeof(float)) == 0;
#endif
        if (slot.locked) m_lockedBytes.fetch_add(kPageSamples * sizeof(float), std::memory_order_relaxed);
    }
    slot.source.store(id, std::memory_order_relaxed); slot.page.store(page, std::memory_order_relaxed);
    // A warm-up can fill many pages within one worker tick. Giving them the
    // same age repeatedly selected the same tied victim, evicting pages that
    // had just been warmed instead of the older residents.
    slot.used.store(m_useClock.fetch_add(1, std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
    slot.prefetched.store(false, std::memory_order_relaxed);
    slot.readers.fetch_sub(Slot::kWriter, std::memory_order_release);
}
void PcmReadCache::warm(std::uint64_t id, std::size_t first, std::size_t count) {
    const std::lock_guard lock(m_fillMutex);
    const auto source = findSource(id);
    if (!source.data || first >= source.samples || count == 0) return;
    const auto end = first + std::min(count, source.samples - first);
    for (auto page = first / kPageSamples; page <= (end - 1) / kPageSamples; ++page) fill(id, page, source);
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
            if (request.epoch != m_epoch.load(std::memory_order_acquire)) { finishRequest(request); m_stale.fetch_add(1); continue; }
            const std::lock_guard lock(m_fillMutex);
            // Resolve under the fill lock so removal cannot be followed by a
            // delayed fill that resurrects pages for the retired source.
            const auto source = findSource(request.source);
            if (!source.data) { finishRequest(request); continue; }
            for (unsigned ahead = 0; ahead < 4; ++ahead) {
                if (request.epoch != m_epoch.load(std::memory_order_acquire)) break;
                fill(request.source, request.page + ahead, source);
            }
            finishRequest(request);
        }
        if (m_retirementsPending.load(std::memory_order_acquire)) {
            const std::lock_guard lock(m_fillMutex);
            std::erase_if(m_retiredSources, [this](auto id) { return releaseSourcePages(id); });
            m_retirementsPending.store(!m_retiredSources.empty(), std::memory_order_release);
        }
        if (!worked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
PcmReadCache::Counters PcmReadCache::counters() const noexcept {
    return {m_misses.load(), m_dropped.load(), m_stale.load(), m_count * kPageSamples * sizeof(float),
        m_lockedBytes.load(std::memory_order_relaxed), m_allocatedBytes.load(std::memory_order_relaxed)};
}
PcmReadScope::PcmReadScope(bool realtime) noexcept : m_previous(s_current) { s_current = realtime ? this : nullptr; }
PcmReadScope::~PcmReadScope() { for (unsigned i = 0; i < m_pinCount; ++i) release(m_pins[i]); s_current = m_previous; }
void PcmReadScope::release(Pin& pin) noexcept {
    if (pin.cache && pin.slot < pin.cache->m_count) pin.cache->m_slots[pin.slot].readers.fetch_sub(1, std::memory_order_release);
    pin = {};
}
std::span<const float> PcmReadScope::view(PcmReadCache& cache, std::uint64_t source,
                                       std::size_t sample, std::size_t count) noexcept {
    const auto page = sample / PcmReadCache::kPageSamples;
    auto matches = [&](const Pin& pin) { return pin.cache == &cache && pin.source == source && pin.page == page; };
    if (m_pinCount == 0 || !matches(m_pins[m_last])) {
        unsigned found = m_pinCount;
        for (unsigned i = 0; i < m_pinCount; ++i) if (matches(m_pins[i])) { found = i; break; }
        if (found == m_pinCount) {
            if (m_pinCount < m_pins.size()) found = m_pinCount++;
            else {
                found = m_next++ % unsigned(m_pins.size());
                release(m_pins[found]);
            }
            auto& pin = m_pins[found];
            const auto slot = cache.pin(source, page);
            pin = {&cache, source, page, slot, slot < cache.m_count ? cache.m_pcm + slot * PcmReadCache::kPageSamples : s_silence.data()};
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
    auto& pin = m_pins[m_last];
    if (pin.slot == cache.m_count) {
        const auto tick = cache.m_clock.load(std::memory_order_relaxed);
        if (tick != pin.retryTick) {
            pin.retryTick = tick;
            const auto slot = cache.pin(source, page);
            if (slot < cache.m_count) {
                pin.slot = slot;
                pin.data = cache.m_pcm + slot * PcmReadCache::kPageSamples;
            }
        }
    }
    const auto offset = sample % PcmReadCache::kPageSamples;
    return {pin.data + offset, std::min(count, PcmReadCache::kPageSamples - offset)};
}
} // namespace daw::engine
