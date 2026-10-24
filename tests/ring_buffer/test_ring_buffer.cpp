#include <catch2/catch_test_macros.hpp>

#include <cppgate/RingBuffer.h>

#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("RingBuffer push and pop in order", "[ring_buffer]")
{
    RingBuffer<int> rb(4);

    int out = 0;
    CHECK_FALSE(rb.pop(out));

    for (int i = 0; i < 4; ++i)
        REQUIRE(rb.push(i));

    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(rb.pop(out));
        CHECK(out == i);
    }

    CHECK_FALSE(rb.pop(out));
    CHECK(rb.empty());
}

TEST_CASE("RingBuffer loses and duplicates nothing under concurrent pop/push", "[ring_buffer][threads]")
{
    // Used like the router's params pool: every thread takes one item and gives it back.
    // pop() may report empty while a push is in progress, so failures are allowed, losses are not.
    constexpr int capacity = 16;
    constexpr int threads = 8;
    constexpr int rounds = 200000;

    RingBuffer<int> rb(capacity);
    for (int i = 0; i < capacity; ++i)
        rb.push(i);

    std::atomic<int> successful_pops{ 0 };
    std::vector<std::thread> workers;

    for (int t = 0; t < threads; ++t)
    {
        workers.emplace_back([&] {
            for (int i = 0; i < rounds; ++i)
            {
                int item;
                if (!rb.pop(item))
                    continue;
                ++successful_pops;
                rb.push(item);
            }
        });
    }

    for (auto& worker : workers)
        worker.join();

    CHECK(successful_pops > 0);

    // Every item is still there exactly once
    std::vector<bool> seen(capacity, false);
    int item;
    int count = 0;
    while (rb.pop(item))
    {
        REQUIRE(item >= 0);
        REQUIRE(item < capacity);
        CHECK_FALSE(seen[item]);
        seen[item] = true;
        ++count;
    }
    CHECK(count == capacity);
}
