#pragma once
//
// Публикация иммутабельных данных из UI-потока в аудио-поток без блокировок.
// Реализация схемы из ARCHITECTURE.md §5.3 в переиспользуемом виде.
//
// Задача: аудио-поток должен читать структуру (карту темпа, а позже — весь
// аудио-граф), которую UI-поток время от времени заменяет целиком. Мьютекс
// запрещён, а просто удалить старый объект нельзя — аудио-поток может быть
// внутри него прямо сейчас.
//
// Решение: указатель подменяется атомарно, старый объект уходит в список
// отставленных и освобождается позже — когда счётчик эпох докажет, что
// аудио-поток гарантированно вышел из него и уже видел новый указатель.
//
// Счётчик эпох растёт на 1 на входе в критическую секцию и на 1 на выходе,
// то есть нечётное значение означает «аудио-поток внутри». Освобождать
// отставленный объект безопасно, когда эпоха продвинулась минимум на 2 от
// момента отставки: этого достаточно, чтобы любой читатель успел выйти
// и на следующем входе взять уже новый указатель.
//
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace daw::rt {

template <typename T>
class RcuPublisher {
public:
    RcuPublisher() = default;

    explicit RcuPublisher(std::unique_ptr<T> initial) noexcept
        : live_(initial.release()) {}

    ~RcuPublisher() {
        delete live_.load(std::memory_order_relaxed);
        for (auto& r : retired_)
            delete r.first;
    }

    RcuPublisher(const RcuPublisher&) = delete;
    RcuPublisher& operator=(const RcuPublisher&) = delete;

    // ---- UI-поток ---------------------------------------------------------

    // Опубликовать новую версию. Старая уходит в отставку, но не удаляется.
    void publish(std::unique_ptr<T> fresh) {
        T* old = live_.exchange(fresh.release(), std::memory_order_acq_rel);
        if (old)
            retired_.emplace_back(old, epoch_.load(std::memory_order_acquire));
        collect();
    }

    // Освободить те отставленные версии, которых аудио-поток точно не видит.
    // Вызывать периодически — например, по таймеру телеметрии.
    void collect() {
        const std::uint64_t now = epoch_.load(std::memory_order_acquire);

        // Чётная эпоха означает, что ни один читатель не находится внутри
        // критической секции, а значит никто не держит отставленный указатель:
        // вошедший позже прочитает live_ заново и увидит уже новую версию.
        //
        // Проверять это отдельно необходимо: правила «эпоха продвинулась на 2»
        // недостаточно, когда читателя вообще нет. Если устройство не открыто
        // или транспорт стоит, callback'и не идут, эпоха не растёт — и без
        // этой ветки отставленные версии копились бы до конца сеанса.
        const bool noReaderInside = (now % 2) == 0;

        std::size_t keep = 0;
        for (std::size_t i = 0; i < retired_.size(); ++i) {
            if (noReaderInside || now >= retired_[i].second + 2) {
                delete retired_[i].first;
            } else {
                retired_[keep++] = retired_[i];
            }
        }
        retired_.resize(keep);
    }

    // Текущая версия для чтения из UI-потока (он же единственный писатель).
    T* current() noexcept { return live_.load(std::memory_order_acquire); }
    const T* current() const noexcept { return live_.load(std::memory_order_acquire); }

    std::size_t retiredCount() const noexcept { return retired_.size(); }

    // ---- Аудио-поток ------------------------------------------------------

    // RAII-доступ к текущей версии. Внутри области действия указатель жив.
    class ReadGuard {
    public:
        explicit ReadGuard(RcuPublisher& owner) noexcept : owner_(owner) {
            owner_.epoch_.fetch_add(1, std::memory_order_acq_rel);   // вошли: нечётное
            ptr_ = owner_.live_.load(std::memory_order_acquire);
        }
        ~ReadGuard() noexcept {
            owner_.epoch_.fetch_add(1, std::memory_order_acq_rel);   // вышли: чётное
        }
        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;

        const T* get() const noexcept { return ptr_; }
        const T* operator->() const noexcept { return ptr_; }
        explicit operator bool() const noexcept { return ptr_ != nullptr; }

    private:
        RcuPublisher& owner_;
        const T*      ptr_ = nullptr;
    };

private:
    std::atomic<T*>            live_{nullptr};
    std::atomic<std::uint64_t> epoch_{0};

    // Живёт только в UI-потоке, поэтому обычный вектор без синхронизации.
    std::vector<std::pair<T*, std::uint64_t>> retired_;
};

} // namespace daw::rt
