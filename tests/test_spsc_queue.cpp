#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include "daw/rt/SpscQueue.h"

using daw::rt::SpscQueue;

TEST_CASE("SpscQueue: empty queue yields nothing", "[rt][queue]") {
    SpscQueue<int, 8> q;
    int value = 123;

    REQUIRE(q.empty());
    REQUIRE(q.size() == 0);
    REQUIRE_FALSE(q.pop(value));
    REQUIRE(value == 123);   // pop не должен трогать выход при неудаче
}

TEST_CASE("SpscQueue: preserves FIFO order", "[rt][queue]") {
    SpscQueue<int, 8> q;

    for (int i = 0; i < 5; ++i)
        REQUIRE(q.push(i));

    REQUIRE(q.size() == 5);

    for (int i = 0; i < 5; ++i) {
        int v = -1;
        REQUIRE(q.pop(v));
        REQUIRE(v == i);
    }
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue: fills exactly to capacity", "[rt][queue]") {
    SpscQueue<int, 4> q;

    REQUIRE(q.capacity() == 4);
    for (int i = 0; i < 4; ++i)
        REQUIRE(q.push(i));

    // Пятый элемент не влезает — очередь обязана отказать, а не перезаписать.
    // Молчаливая перезапись потеряла бы команду транспорта.
    REQUIRE_FALSE(q.push(99));
    REQUIRE(q.size() == 4);

    int v = 0;
    REQUIRE(q.pop(v));
    REQUIRE(v == 0);
    REQUIRE(q.push(99));     // место освободилось
}

TEST_CASE("SpscQueue: wraps around repeatedly", "[rt][queue]") {
    SpscQueue<int, 4> q;

    // Прогоняем на порядок больше элементов, чем ёмкость: ловим ошибки
    // в маскировании индексов.
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(q.push(i));
        int v = -1;
        REQUIRE(q.pop(v));
        REQUIRE(v == i);
    }
    REQUIRE(q.empty());
}

TEST_CASE("SpscQueue: loses nothing under contention", "[rt][queue][stress]") {
    constexpr int kCount = 200000;
    SpscQueue<std::uint64_t, 1024> q;

    std::atomic<bool> producerDone{false};
    std::vector<std::uint64_t> received;
    received.reserve(kCount);

    std::thread producer([&] {
        for (int i = 0; i < kCount; ++i) {
            // Очередь маленькая, писатель заведомо обгонит читателя —
            // крутимся, пока не освободится место.
            while (!q.push(static_cast<std::uint64_t>(i)))
                std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::uint64_t v = 0;
    while (received.size() < static_cast<std::size_t>(kCount)) {
        if (q.pop(v))
            received.push_back(v);
        else if (producerDone.load(std::memory_order_acquire) && q.empty())
            break;
    }
    producer.join();

    REQUIRE(received.size() == static_cast<std::size_t>(kCount));
    for (int i = 0; i < kCount; ++i)
        REQUIRE(received[static_cast<std::size_t>(i)] == static_cast<std::uint64_t>(i));
}
