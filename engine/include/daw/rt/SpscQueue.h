#pragma once
//
// Кольцевая очередь на одного писателя и одного читателя, без блокировок.
// Основной канал связи UI-поток → аудио-поток (команды) и обратно (события).
//
// Ограничения намеренные:
//   * ровно один поток-писатель и ровно один поток-читатель;
//   * T должен быть тривиально копируемым — никаких std::string и shared_ptr;
//   * размер — степень двойки, чтобы индекс сводился к маске.
//
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#if defined(_MSC_VER)
    // C4324: структура дополнена из-за alignas. Здесь это цель, а не проблема:
    // индексы намеренно разнесены по строкам кэша.
    #pragma warning(push)
    #pragma warning(disable : 4324)
#endif

namespace daw::rt {

// Размер строки кэша. hardware_destructive_interference_size поддержан не везде,
// а 64 верно для x86-64 и для Apple Silicon (там 128, но 64 тоже безопасно —
// просто чуть больше выравнивания, чем нужно).
inline constexpr std::size_t kCacheLine = 64;

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "В аудио-поток можно передавать только тривиально копируемые типы");
    static_assert(Capacity >= 2, "Слишком малая ёмкость");
    static_assert((Capacity & (Capacity - 1)) == 0, "Ёмкость должна быть степенью двойки");

public:
    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Вызывается только потоком-писателем. Возвращает false, если очередь полна.
    bool push(const T& value) noexcept {
        const std::uint64_t w = write_.load(std::memory_order_relaxed);
        const std::uint64_t r = read_.load(std::memory_order_acquire);
        if (w - r >= Capacity)
            return false;                       // переполнение — это баг вызывающего
        data_[w & kMask] = value;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Вызывается только потоком-читателем. Возвращает false, если очередь пуста.
    bool pop(T& out) noexcept {
        const std::uint64_t r = read_.load(std::memory_order_relaxed);
        const std::uint64_t w = write_.load(std::memory_order_acquire);
        if (r == w)
            return false;
        out = data_[r & kMask];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }

    // Приблизительный размер: точен только для потока, который сам его меняет.
    std::size_t size() const noexcept {
        const std::uint64_t w = write_.load(std::memory_order_acquire);
        const std::uint64_t r = read_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(w - r);
    }

    bool empty() const noexcept { return size() == 0; }
    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    // Индексы разнесены по разным строкам кэша: иначе писатель и читатель
    // будут гонять одну строку между ядрами (false sharing) и очередь
    // станет медленнее мьютекса.
    alignas(kCacheLine) std::atomic<std::uint64_t> write_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> read_{0};
    alignas(kCacheLine) T data_[Capacity]{};
};

} // namespace daw::rt

#if defined(_MSC_VER)
    #pragma warning(pop)
#endif
