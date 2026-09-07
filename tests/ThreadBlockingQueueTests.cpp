// SPDX-License-Identifier: GPL-2.0+

#include "TestHarness.h"
#include "ThreadBlockingQueue.h"

#include <atomic>
#include <thread>

CUBIC_TEST(blocking_queue_preserves_fifo_order) {
    ThreadBlockingQueue<int> queue;
    queue.set_max_num_items(3);

    CUBIC_REQUIRE(queue.try_push(10));
    CUBIC_REQUIRE(queue.try_push(20));
    CUBIC_REQUIRE(queue.try_push(30));

    int value = 0;
    CUBIC_REQUIRE(queue.try_pop(value));
    CUBIC_REQUIRE(value == 10);
    CUBIC_REQUIRE(queue.try_pop(value));
    CUBIC_REQUIRE(value == 20);
    CUBIC_REQUIRE(queue.try_pop(value));
    CUBIC_REQUIRE(value == 30);
    CUBIC_REQUIRE(queue.empty());
}

CUBIC_TEST(blocking_queue_rejects_non_blocking_push_when_full) {
    ThreadBlockingQueue<int> queue;

    CUBIC_REQUIRE(queue.try_push(1));
    CUBIC_REQUIRE(queue.full());
    CUBIC_REQUIRE(!queue.try_push(2));
    CUBIC_REQUIRE(queue.size() == 1);
}

CUBIC_TEST(blocking_queue_flush_clears_items) {
    ThreadBlockingQueue<int> queue;
    queue.set_max_num_items(2);
    CUBIC_REQUIRE(queue.try_push(1));
    CUBIC_REQUIRE(queue.try_push(2));

    queue.flush();

    CUBIC_REQUIRE(queue.empty());
    CUBIC_REQUIRE(queue.try_push(3));
}

CUBIC_TEST(blocking_queue_timed_pop_reports_an_empty_queue) {
    ThreadBlockingQueue<int> queue;
    int value = 0;

    CUBIC_REQUIRE(!queue.pop(value, 2'000));
}

CUBIC_TEST(blocking_queue_unblocks_a_waiting_producer) {
    ThreadBlockingQueue<int> queue;
    CUBIC_REQUIRE(queue.try_push(1));

    std::atomic_bool producerStarted(false);
    bool producerResult = false;
    std::thread producer([&]() {
        producerStarted.store(true);
        producerResult = queue.push(2, 500'000);
    });

    while (!producerStarted.load()) {
        std::this_thread::yield();
    }

    int value = 0;
    const bool consumerResult = queue.pop(value, 500'000);
    const int firstValue = value;
    producer.join();

    CUBIC_REQUIRE(consumerResult);
    CUBIC_REQUIRE(firstValue == 1);
    CUBIC_REQUIRE(producerResult);
    CUBIC_REQUIRE(queue.try_pop(value));
    CUBIC_REQUIRE(value == 2);
}
