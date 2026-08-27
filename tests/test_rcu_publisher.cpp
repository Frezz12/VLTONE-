#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <thread>

#include "daw/rt/RcuPublisher.h"

using daw::rt::RcuPublisher;

namespace {

// Магическое число позволяет заметить чтение уже освобождённой памяти:
// деструктор его затирает, и читатель увидит мусор вместо kAlive.
constexpr int kAlive = 0x5A5A5A5A;

struct Payload {
    int magic = kAlive;
    int value = 0;

    explicit Payload(int v) : value(v) { ++liveCount; }
    ~Payload() { magic = 0xDEAD; --liveCount; }

    static std::atomic<int> liveCount;
};

std::atomic<int> Payload::liveCount{0};

} // namespace

TEST_CASE("RcuPublisher: publishes and reads the current version", "[rt][rcu]") {
    RcuPublisher<Payload> pub(std::make_unique<Payload>(1));

    REQUIRE(pub.current()->value == 1);

    RcuPublisher<Payload>::ReadGuard guard(pub);
    REQUIRE(guard);
    REQUIRE(guard->value == 1);
}

TEST_CASE("RcuPublisher: does not free a version being read", "[rt][rcu]") {
    Payload::liveCount.store(0);
    {
        RcuPublisher<Payload> pub(std::make_unique<Payload>(1));
        REQUIRE(Payload::liveCount.load() == 1);

        {
            RcuPublisher<Payload>::ReadGuard guard(pub);
            REQUIRE(guard->value == 1);

            // Публикация во время чтения: старая версия уходит в отставку,
            // но освободить её сейчас нельзя — читатель внутри неё.
            pub.publish(std::make_unique<Payload>(2));

            pub.collect();
            REQUIRE(pub.retiredCount() == 1);
            REQUIRE(Payload::liveCount.load() == 2);

            // Читатель, вошедший до подмены, продолжает видеть старую версию.
            REQUIRE(guard->magic == kAlive);
            REQUIRE(guard->value == 1);
        }

        // Новые читатели видят новую версию.
        {
            RcuPublisher<Payload>::ReadGuard guard(pub);
            REQUIRE(guard->value == 2);
        }

        pub.collect();
        REQUIRE(pub.retiredCount() == 0);
        REQUIRE(Payload::liveCount.load() == 1);
    }
    REQUIRE(Payload::liveCount.load() == 0);
}

TEST_CASE("RcuPublisher: frees everything on destruction", "[rt][rcu]") {
    Payload::liveCount.store(0);
    {
        RcuPublisher<Payload> pub(std::make_unique<Payload>(1));
        // Отставленные версии, которые collect() ещё не забрал, тоже должны
        // освобождаться — иначе смена темпа станет утечкой.
        RcuPublisher<Payload>::ReadGuard guard(pub);
        pub.publish(std::make_unique<Payload>(2));
        REQUIRE(pub.retiredCount() == 1);
    }
    REQUIRE(Payload::liveCount.load() == 0);
}

TEST_CASE("RcuPublisher: reclaims when no reader ever runs", "[rt][rcu]") {
    // Регрессия. Правило «эпоха продвинулась на два» не срабатывает, если
    // читателя нет вовсе: аудио-устройство не открыто или транспорт стоит,
    // callback'и не идут, эпоха стоит на месте. Раньше в этом состоянии каждое
    // движение регулятора темпа копило неосвобождаемую копию карты до конца
    // сеанса. Чётная эпоха доказывает, что внутри никого нет.
    Payload::liveCount.store(0);
    {
        RcuPublisher<Payload> pub(std::make_unique<Payload>(0));

        for (int i = 1; i <= 500; ++i)
            pub.publish(std::make_unique<Payload>(i));

        // publish() сам вызывает collect(), так что накопиться не должно
        // ничего: жив только текущий объект.
        REQUIRE(pub.retiredCount() == 0);
        REQUIRE(Payload::liveCount.load() == 1);
        REQUIRE(pub.current()->value == 500);
    }
    REQUIRE(Payload::liveCount.load() == 0);
}

TEST_CASE("RcuPublisher: reader never observes freed memory", "[rt][rcu][stress]") {
    Payload::liveCount.store(0);

    RcuPublisher<Payload> pub(std::make_unique<Payload>(0));

    std::atomic<bool> stop{false};
    std::atomic<int>  corrupted{0};
    std::atomic<long> reads{0};

    // Изображает аудио-поток: непрерывно берёт ReadGuard и проверяет,
    // что видит живой объект.
    std::thread reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            RcuPublisher<Payload>::ReadGuard guard(pub);
            if (!guard || guard->magic != kAlive)
                corrupted.fetch_add(1, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Изображает UI-поток: часто публикует новые версии.
    for (int i = 1; i <= 20000; ++i) {
        pub.publish(std::make_unique<Payload>(i));
    }

    stop.store(true, std::memory_order_relaxed);
    reader.join();

    INFO("прочитано версий: " << reads.load());
    REQUIRE(corrupted.load() == 0);

    // После остановки читателя всё лишнее должно освобождаться.
    pub.collect();
    pub.collect();
    REQUIRE(Payload::liveCount.load() == 1 + static_cast<int>(pub.retiredCount()));
}
