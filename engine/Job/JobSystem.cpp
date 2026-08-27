#include "Job/JobSystem.hpp"

#include <algorithm>
#include <chrono>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

#if defined(__APPLE__)
    #include <pthread.h>
    #include <pthread/qos.h>
#endif

namespace daw::engine {

namespace {
/// Cheap pseudo-random victim choice; a fixed rotation would make every thief
/// converge on the same deque.
inline unsigned nextVictim(unsigned& state, unsigned count) noexcept {
    state = state * 1664525u + 1013904223u;
    return state % count;
}

inline void pause() noexcept {
#if defined(__ARM_ARCH) || defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#endif
}

/// Cores worth putting a realtime pass on — every logical core, including the
/// efficiency cluster.
///
/// Restricting the pool to the performance cores was measured and rejected: on
/// this 4P+4E machine the 1000-track benchmark went from 3.7 ms to 5.16 ms per
/// block. A work-stealing scheduler already copes with uneven cores — a slow one
/// simply steals fewer nodes — so the extra cluster is throughput gained, not
/// a straggler introduced. Placement is left to the QoS class below.
unsigned schedulableCores() noexcept {
    return std::max(1u, std::thread::hardware_concurrency());
}

/// Tell the OS this thread carries audio, so it is scheduled promptly and
/// preferentially on a performance core instead of being treated as background
/// work. Without this the pool inherits the default QoS and a pass can be left
/// waiting on a worker the scheduler has parked on an efficiency core.
void markAsAudioWorker() noexcept {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

/// How long a pool worker keeps hunting for work inside an unfinished pass
/// before parking. Long enough to cover the gap while another worker finishes
/// the node it is blocked on, short enough that a worker never burns a core for
/// a meaningful slice of the block period.
constexpr auto kIdleGiveUp = std::chrono::microseconds(60);
/// Spins between clock reads — `steady_clock::now()` is cheap but not free.
constexpr unsigned kSpinsPerClockCheck = 256;
} // namespace

JobSystem::JobSystem(unsigned threadCount) {
    m_workerCount = threadCount > 0 ? threadCount : schedulableCores();
    m_workerCount = std::clamp(m_workerCount, 1u, 128u);

    m_workers = std::vector<Worker>(m_workerCount);

    // Worker 0 is the calling (audio) thread — it does not get a std::thread.
    for (unsigned i = 1; i < m_workerCount; ++i) {
        m_threads.emplace_back([this, i] { workerLoop(i); });
    }
}

JobSystem::~JobSystem() {
    // seq_cst to match the load in park(): the two together decide whether a
    // worker about to sleep notices the shutdown or has to be woken out of it.
    m_running.store(false, std::memory_order_seq_cst);
    // Bump the generation *and* wake unconditionally: the workers are parked,
    // and a shutdown that skipped the wake would hang in join().
    m_generation.fetch_add(1, std::memory_order_seq_cst);
    m_generation.notify_all();
    for (auto& thread : m_threads) {
        if (thread.joinable()) thread.join();
    }
}

void JobSystem::prepare(std::size_t itemCapacity) {
    const std::size_t capacity = std::max<std::size_t>(itemCapacity, 64);
    // Grow only. `reserve` swaps the slot array out and zeroes top/bottom, so
    // doing it for a graph that already fits would corrupt a pass in flight for
    // no gain — and publishing a new graph happens on every routing edit.
    if (capacity <= m_itemCapacity) return;
    for (auto& worker : m_workers) worker.deque.reserve(capacity);
    m_itemCapacity = capacity;
}

bool JobSystem::acquireItem(unsigned index, std::uint32_t& item) noexcept {
    if (m_workers[index].deque.pop(item)) return true;
    if (m_workerCount == 1) return false;

    // Work stealing: try a few random victims before giving up for this spin.
    // A few random victims, not a full sweep: idle workers hammering every
    // deque generate more cache traffic than the work they are looking for.
    static thread_local unsigned randomState = 0x9E3779B9u;
    const unsigned attempts = std::min(m_workerCount, 4u);
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        const unsigned victim = nextVictim(randomState, m_workerCount);
        if (victim == index) continue;
        if (m_workers[victim].deque.steal(item)) return true;
    }
    return false;
}

void JobSystem::runUntilPassComplete(unsigned index) noexcept {
    // Worker 0 is the audio thread: it must not leave before the block it is
    // rendering is finished. Pool workers are free to give up and park.
    const bool mustFinish = (index == 0);

    // Completions are counted locally and flushed in batches: one shared atomic
    // incremented by every node turns a 3000-node graph into 3000 contended
    // writes to a single cache line, which costs more than the DSP itself.
    std::uint64_t local = 0;
    constexpr std::uint64_t kFlushEvery = 32;

    unsigned idleSpins = 0;
    auto idleSince = std::chrono::steady_clock::time_point{};

    // The target is re-read every turn, so a worker that is still here when the
    // next pass opens simply keeps working instead of dropping out and back in.
    while (m_completed.load(std::memory_order_acquire) + local <
           m_target.load(std::memory_order_acquire)) {
        std::uint32_t item = 0;
        if (acquireItem(index, item)) {
            idleSpins = 0;
            idleSince = {};
            m_sink.execute(m_sink.context, item, index);
            if (++local >= kFlushEvery) {
                m_completed.fetch_add(local, std::memory_order_release);
                local = 0;
            }
            continue;
        }
        // Out of work: publish what we have done so the others can see the pass
        // progress, then wait.
        if (local > 0) {
            m_completed.fetch_add(local, std::memory_order_release);
            local = 0;
        }
        // Nothing to run yet: the remaining nodes are still blocked on
        // dependencies. Spin — latency matters far more than the cycles — but
        // only for a bounded stretch. A pool worker that has been empty-handed
        // past the budget returns to park; the pass still completes, because
        // worker 0 never leaves and a departing worker's deque is empty by
        // construction (only its owner can push to it).
        if (++idleSpins >= kSpinsPerClockCheck) {
            idleSpins = 0;
            const auto now = std::chrono::steady_clock::now();
            if (idleSince == std::chrono::steady_clock::time_point{}) {
                idleSince = now;
            } else if (now - idleSince >= kIdleGiveUp) {
                if (!mustFinish) return;
                // The audio thread has to stay, so stop hot-spinning and let
                // the scheduler run whoever still owes us a node.
                std::this_thread::yield();
                idleSince = now;
            }
        }
        pause();
    }
    if (local > 0) m_completed.fetch_add(local, std::memory_order_release);
}

void JobSystem::park(unsigned index, std::uint64_t lastGeneration) noexcept {
    (void)index;
    // Announce the park *before* re-reading the generation, and make both
    // operations seq_cst. beginPass does the mirror image — bump the generation,
    // then read the park count — so in the single total order at least one side
    // sees the other: either we notice the new pass and skip the park, or the
    // dispatcher notices us and sends the wake.
    m_parked.fetch_add(1, std::memory_order_seq_cst);
    if (m_generation.load(std::memory_order_seq_cst) == lastGeneration &&
        m_running.load(std::memory_order_seq_cst)) {
        // Blocks in the kernel until notify_all/notify_one. atomic::wait
        // re-checks the value itself, so a change that lands right here is not
        // lost either.
        m_generation.wait(lastGeneration, std::memory_order_acquire);
    }
    m_parked.fetch_sub(1, std::memory_order_seq_cst);
}

void JobSystem::workerLoop(unsigned index) {
    markAsAudioWorker();

    std::uint64_t lastGeneration = 0;
    while (m_running.load(std::memory_order_acquire)) {
        const std::uint64_t generation = m_generation.load(std::memory_order_acquire);
        if (generation == lastGeneration) {
            park(index, lastGeneration);
            continue;
        }
        lastGeneration = generation;
        if (!m_running.load(std::memory_order_acquire)) break;
        runUntilPassComplete(index);
    }
}

void JobSystem::beginPass(std::uint32_t items, unsigned helpers) noexcept {
    if (items == 0) return;
    m_target.fetch_add(items, std::memory_order_release);
    // Even a caller asking for no helpers opens a new epoch. That keeps pool
    // workers from confusing a later mid-pass wake with the previous pass.
    if (helpers == 0 || m_workerCount == 1) {
        m_generation.fetch_add(1, std::memory_order_seq_cst);
        return;
    }
    wakeHelpers(helpers);
}

void JobSystem::wakeHelpers(unsigned helpers) noexcept {
    if (helpers == 0 || m_workerCount == 1) return;

    // Publishing a new work epoch opens a pass or announces a newly-ready
    // frontier inside one. There is no condition variable, so neither the
    // audio thread nor a DSP worker touches a mutex.
    m_generation.fetch_add(1, std::memory_order_seq_cst);

    // Pair of the Dekker handshake in park(). Waking costs a syscall, so it is
    // skipped whenever nobody is parked — which is the steady state during a
    // dense render, where workers are still inside the previous pass.
    const unsigned parked = m_parked.load(std::memory_order_seq_cst);
    if (parked == 0) return;

    // Only pool workers park; the announcing worker may be worker 0 or one of
    // those pool workers.
    const unsigned wanted = std::min(helpers, m_workerCount - 1);
    if (wanted >= parked) {
        m_generation.notify_all();
    } else {
        for (unsigned i = 0; i < wanted; ++i) m_generation.notify_one();
    }
}

} // namespace daw::engine
