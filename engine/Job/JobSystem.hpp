#pragma once

#include "Common/Types.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace daw::engine {

/// A bounded Chase-Lev work-stealing deque holding node indices.
///
/// The owning worker pushes and pops at the bottom (LIFO — the node it just
/// unblocked is the one whose data is still in cache); thieves take from the
/// top. Bounded and preallocated, so nothing allocates while audio runs.
class WorkStealingDeque {
public:
    void reserve(std::size_t capacity) {
        // Round up to a power of two so the index wrap is a mask.
        std::size_t size = 1;
        while (size < capacity) size <<= 1;
        m_mask = size - 1;
        m_slots = std::make_unique<std::atomic<std::uint32_t>[]>(size);
        reset();
    }

    void reset() noexcept {
        m_bottom.store(0, std::memory_order_relaxed);
        m_top.store(0, std::memory_order_relaxed);
    }

    /// Owner only.
    void push(std::uint32_t value) noexcept {
        const std::int64_t bottom = m_bottom.load(std::memory_order_relaxed);
        m_slots[std::size_t(bottom) & m_mask].store(value, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        m_bottom.store(bottom + 1, std::memory_order_relaxed);
    }

    /// Owner only. Returns false when the deque is empty.
    bool pop(std::uint32_t& out) noexcept {
        std::int64_t bottom = m_bottom.load(std::memory_order_relaxed) - 1;
        m_bottom.store(bottom, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t top = m_top.load(std::memory_order_relaxed);

        if (top > bottom) {                      // empty
            m_bottom.store(bottom + 1, std::memory_order_relaxed);
            return false;
        }
        out = m_slots[std::size_t(bottom) & m_mask].load(std::memory_order_relaxed);
        if (top != bottom) return true;          // more than one item left

        // Last item: race against a thief for it.
        const bool won = m_top.compare_exchange_strong(top, top + 1,
                                                       std::memory_order_seq_cst,
                                                       std::memory_order_relaxed);
        m_bottom.store(bottom + 1, std::memory_order_relaxed);
        return won;
    }

    /// Any thread. Returns false when empty or when another thief won the race.
    bool steal(std::uint32_t& out) noexcept {
        std::int64_t top = m_top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const std::int64_t bottom = m_bottom.load(std::memory_order_acquire);
        if (top >= bottom) return false;

        out = m_slots[std::size_t(top) & m_mask].load(std::memory_order_relaxed);
        return m_top.compare_exchange_strong(top, top + 1,
                                             std::memory_order_seq_cst,
                                             std::memory_order_relaxed);
    }

private:
    std::unique_ptr<std::atomic<std::uint32_t>[]> m_slots;
    std::size_t m_mask = 0;
    alignas(kCacheLine) std::atomic<std::int64_t> m_top{0};
    alignas(kCacheLine) std::atomic<std::int64_t> m_bottom{0};
};

/// What a worker does with a node index it pulled off a deque. The graph
/// processor installs this; keeping it a plain function pointer + context keeps
/// the realtime path free of std::function's indirection and allocation.
struct JobSink {
    void (*execute)(void* context, std::uint32_t item, unsigned workerIndex) = nullptr;
    void* context = nullptr;
};

/// Fixed thread pool that runs one dependency-driven pass at a time.
///
/// The audio thread is worker 0: it dispatches, works alongside the pool, and
/// returns when the pass is complete. Between passes the pool threads are
/// **parked** on the generation counter — a DAW spends most of its life with no
/// pass open, and a pool that spin-waits through those gaps burns a core per
/// worker doing nothing. Inside a pass a worker spins briefly before parking
/// again, so the frequent case (work is one dependency away) still costs no
/// syscall. Nothing here locks or allocates once `prepare()` has run.
class JobSystem {
public:
    /// `threadCount` = 0 asks the hardware; the pool then owns
    /// threadCount − 1 background workers plus the calling (audio) thread.
    explicit JobSystem(unsigned threadCount = 0);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    unsigned workerCount() const noexcept { return m_workerCount; }

    /// Size the per-worker deques. Control thread only, and **only while no
    /// pass can be running** — growing reallocates the slot array the workers
    /// are indexing and resets their top/bottom, which loses items and hangs
    /// the pass that was open. Capacity only ever grows, so a rebuild that
    /// needs no more room is a no-op and is safe to call at any time; ask
    /// `needsGrowthFor` first when a pass might be in flight.
    void prepare(std::size_t itemCapacity);

    /// True when `prepare(itemCapacity)` would reallocate.
    bool needsGrowthFor(std::size_t itemCapacity) const noexcept {
        return std::max<std::size_t>(itemCapacity, 64) > m_itemCapacity;
    }

    void setSink(JobSink sink) noexcept { m_sink = sink; }

    /// Push a ready item from inside a job (worker) or before dispatch (audio
    /// thread, worker index 0).
    void submit(unsigned workerIndex, std::uint32_t item) noexcept {
        m_workers[workerIndex].deque.push(item);
    }

    /// Open a pass of `items` jobs and wake up to `helpers` parked workers.
    /// Seed work with `submit(0, …)` afterwards — never before: a worker still
    /// running the previous pass may steal a seeded item the moment it is
    /// visible, and it must be counted against this pass's target.
    ///
    /// `helpers` is a request, not a guarantee: it is clamped to the pool size,
    /// and workers that are already awake need no waking. Passing the width the
    /// graph can actually use keeps a narrow pass from waking eight threads that
    /// find nothing to steal and park again.
    void beginPass(std::uint32_t items, unsigned helpers) noexcept;

    /// Wake parked helpers when a running job exposes a new ready frontier.
    /// This uses the same lock-free generation/atomic-wait handshake as
    /// beginPass(); it neither allocates nor takes a mutex on the realtime
    /// path. `helpers` excludes the worker that exposed the frontier.
    void wakeHelpers(unsigned helpers) noexcept;

    /// Work alongside the pool until the open pass is finished. Audio thread.
    void waitForPass() noexcept { runUntilPassComplete(0); }

    /// beginPass + waitForPass for callers that seed nothing.
    void dispatch(std::uint32_t items) noexcept {
        beginPass(items, m_workerCount);
        waitForPass();
    }

    /// Signal completion of one item from inside a job.
    void notifyCompleted() noexcept {
        m_completed.fetch_add(1, std::memory_order_release);
    }

private:
    struct alignas(kCacheLine) Worker {
        WorkStealingDeque deque;
    };

    void workerLoop(unsigned index);
    /// Take an item from our own deque, else steal from someone else.
    bool acquireItem(unsigned index, std::uint32_t& item) noexcept;
    /// Drain items until the pass finishes; used by pool threads and worker 0.
    /// Worker 0 stays until the pass is complete — it owns the block. A pool
    /// worker that has found nothing for a while returns so it can park.
    void runUntilPassComplete(unsigned index) noexcept;
    /// Park until a new pass or a new mid-pass frontier is announced. Pool
    /// workers only.
    void park(unsigned index, std::uint64_t lastGeneration) noexcept;

    unsigned m_workerCount = 1;
    /// Slots reserved per deque. Monotonic, so the common rebuild — same
    /// project, same node count or fewer — never reallocates.
    std::size_t m_itemCapacity = 0;
    std::vector<Worker> m_workers;
    std::vector<std::thread> m_threads;

    JobSink m_sink;
    // Both counters are monotonic across passes. Resetting them per block used
    // to lose the odd completion when a worker from the previous pass picked up
    // an item of the next one, and the pass would then never finish.
    alignas(kCacheLine) std::atomic<std::uint64_t> m_completed{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> m_target{0};
    // Work-availability epoch. It advances both when a pass opens and when a
    // job exposes enough mid-pass parallelism to justify waking parked helpers.
    alignas(kCacheLine) std::atomic<std::uint64_t> m_generation{0};
    // How many workers are parked on m_generation. Read by whichever worker
    // announces work so it can skip the wake syscall when the pool is already
    // awake, and written by workers on the way in and out of the park. The
    // store/load pairs on both sides are seq_cst: this is a Dekker handshake,
    // and anything weaker lets a worker park just as work appears and sleep
    // through it.
    alignas(kCacheLine) std::atomic<unsigned> m_parked{0};
    std::atomic<bool> m_running{true};
};

} // namespace daw::engine
