#pragma once

#include "Common/Types.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace daw::engine {

/// Single-producer, single-consumer ring. One control thread writes, one audio
/// thread reads (or the reverse), and neither ever blocks the other.
///
/// This is how a parameter change reaches a plugin: not as an atomic the audio
/// thread polls, because every format wants parameter changes as *events* with
/// a frame offset inside the block, and a polled atomic has no timestamp and
/// coalesces away everything but the last value.
///
/// Capacity must be a power of two so the wrap is a mask, and `T` must be
/// trivially copyable because a slot is written and read without any handshake
/// beyond the two indices.
template <typename T, std::size_t Capacity>
class LockFreeSPSCQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T must be trivially copyable");

public:
    /// Producer thread. False when the ring is full — the caller decides
    /// whether to drop or retry, but must never block waiting for room.
    bool push(const T& item) noexcept {
        const std::size_t write = m_writeIndex.load(std::memory_order_relaxed);
        const std::size_t next = (write + 1) & (Capacity - 1);
        if (next == m_readIndex.load(std::memory_order_acquire)) return false;
        m_buffer[write] = item;
        m_writeIndex.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer thread. False when empty.
    bool pop(T& item) noexcept {
        const std::size_t read = m_readIndex.load(std::memory_order_relaxed);
        if (read == m_writeIndex.load(std::memory_order_acquire)) return false;
        item = m_buffer[read];
        m_readIndex.store((read + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return m_readIndex.load(std::memory_order_acquire) ==
               m_writeIndex.load(std::memory_order_acquire);
    }

    /// Consumer thread: throw away everything queued.
    void clear() noexcept {
        m_readIndex.store(m_writeIndex.load(std::memory_order_acquire),
                          std::memory_order_release);
    }

private:
    std::array<T, Capacity> m_buffer{};
    // Separate cache lines: the producer writes one index while the consumer
    // writes the other, and sharing a line would make every push and pop
    // contend for it.
    alignas(kCacheLine) std::atomic<std::size_t> m_readIndex{0};
    alignas(kCacheLine) std::atomic<std::size_t> m_writeIndex{0};
};

/// Bounded multi-producer, single-consumer queue.
///
/// Plugin listeners are allowed to report changes from both their realtime
/// callback and their editor thread. Reserving a slot through a sequence
/// number keeps those producers independent without putting a mutex in either
/// callback. The single consumer may observe the queue as temporarily empty
/// while a producer owns, but has not published, the next slot; it will simply
/// retry on its next drain.
template <typename T, std::size_t Capacity>
class LockFreeMPSCQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T must be trivially copyable");

    struct Cell {
        std::atomic<std::size_t> sequence{0};
        T data{};
    };

public:
    LockFreeMPSCQueue() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i) {
            m_buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    LockFreeMPSCQueue(const LockFreeMPSCQueue&) = delete;
    LockFreeMPSCQueue& operator=(const LockFreeMPSCQueue&) = delete;

    bool push(const T& item) noexcept {
        std::size_t position = m_enqueue.load(std::memory_order_relaxed);
        Cell* cell = nullptr;
        for (;;) {
            cell = &m_buffer[position & (Capacity - 1)];
            const std::size_t sequence =
                cell->sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::ptrdiff_t>(sequence) -
                                    static_cast<std::ptrdiff_t>(position);
            if (difference == 0) {
                if (m_enqueue.compare_exchange_weak(position, position + 1,
                                                    std::memory_order_relaxed)) {
                    break;
                }
            } else if (difference < 0) {
                return false;
            } else {
                position = m_enqueue.load(std::memory_order_relaxed);
            }
        }

        cell->data = item;
        cell->sequence.store(position + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) noexcept {
        Cell& cell = m_buffer[m_dequeue & (Capacity - 1)];
        const std::size_t sequence =
            cell.sequence.load(std::memory_order_acquire);
        if (sequence != m_dequeue + 1) return false;

        item = cell.data;
        cell.sequence.store(m_dequeue + Capacity, std::memory_order_release);
        ++m_dequeue;
        return true;
    }

    bool empty() const noexcept {
        const Cell& cell = m_buffer[m_dequeue & (Capacity - 1)];
        return cell.sequence.load(std::memory_order_acquire) != m_dequeue + 1;
    }

    void clear() noexcept {
        T discarded{};
        while (pop(discarded)) {}
    }

private:
    std::array<Cell, Capacity> m_buffer{};
    alignas(kCacheLine) std::atomic<std::size_t> m_enqueue{0};
    alignas(kCacheLine) std::size_t m_dequeue = 0;
};

} // namespace daw::engine
