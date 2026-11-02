#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cppgate/ConnectionPool.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using gtvr::ConnectionPool;
using gtvr::ConnectionPoolOptions;

namespace {

    // Counts opens and closes, like a real connection would hold a socket
    struct Stats
    {
        std::atomic<int> opened{ 0 };
        std::atomic<int> closed{ 0 };
        std::atomic<int> in_use{ 0 };
        std::atomic<int> max_in_use{ 0 };
        std::atomic<bool> fail_next_open{ false };
        std::atomic<int> validations{ 0 };
        std::atomic<bool> valid{ true };

        int alive() const { return opened - closed; }
    };

    struct FakeConnection
    {
        Stats* stats = nullptr;
        int id = 0;

        FakeConnection(Stats* s, int i) : stats(s), id(i) {}
        FakeConnection(FakeConnection&& other) noexcept : stats(std::exchange(other.stats, nullptr)), id(other.id) {}
        FakeConnection& operator=(FakeConnection&&) = delete;
        ~FakeConnection() { if (stats) ++stats->closed; }
    };

    std::shared_ptr<ConnectionPool<FakeConnection>> makePool(Stats& stats, ConnectionPoolOptions options = {}, bool with_validator = false)
    {
        auto factory = [&stats] {
            if (stats.fail_next_open.exchange(false))
                throw std::runtime_error("database down");
            return FakeConnection(&stats, ++stats.opened);
        };

        ConnectionPool<FakeConnection>::Validator validator;
        if (with_validator)
            validator = [&stats](FakeConnection&) { ++stats.validations; return stats.valid.load(); };

        return ConnectionPool<FakeConnection>::create(options, factory, validator);
    }

    template<typename F>
    std::chrono::milliseconds timed(F&& f)
    {
        auto t0 = std::chrono::steady_clock::now();
        f();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    }

    // Waits until the pool has `n` waiters, so tests don't depend on sleep timing
    template<typename Pool>
    void waitForWaiters(Pool& pool, std::size_t n)
    {
        for (int i = 0; i < 2000 && pool->waiting() < n; ++i)
            std::this_thread::sleep_for(1ms);
        REQUIRE(pool->waiting() == n);
    }

}

TEST_CASE("Connections are opened lazily and reused", "[pool]")
{
    Stats stats;
    auto pool = makePool(stats);
    CHECK(pool->size() == 0);

    int first_id;
    {
        auto conn = pool->acquire();
        first_id = conn->id;
        CHECK(pool->size() == 1);
        CHECK(pool->idle() == 0);
    }
    CHECK(pool->idle() == 1);

    auto again = pool->acquire();
    CHECK(again->id == first_id);
    CHECK(stats.opened == 1);
}

TEST_CASE("The most recently used idle connection is handed out first", "[pool]")
{
    Stats stats;
    auto pool = makePool(stats);

    auto a = pool->acquire();
    auto b = pool->acquire();
    int b_id = b->id;
    a.release();
    b.release();        // returned last

    CHECK(pool->acquire()->id == b_id);
}

TEST_CASE("acquire() times out when max_size connections are in use", "[pool][timeout]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.max_size = 2;
    auto pool = makePool(stats, options);

    auto a = pool->acquire();
    auto b = pool->acquire();

    auto waited = timed([&] { CHECK_THROWS_AS(pool->acquire(100ms), gtvr::ConnectionPoolTimeout); });
    CHECK(waited >= 100ms);
    CHECK(waited < 1000ms);
    CHECK(pool->waiting() == 0);        // the timed out waiter left the queue
    CHECK(stats.opened == 2);

    SECTION("a timeout of 0 doesn't wait")
    {
        auto instant = timed([&] { CHECK_THROWS_AS(pool->acquire(0ms), gtvr::ConnectionPoolTimeout); });
        CHECK(instant < 50ms);
    }
}

TEST_CASE("A waiting thread gets the connection released by another", "[pool]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.max_size = 1;
    auto pool = makePool(stats, options);

    auto held = pool->acquire();
    int held_id = held->id;

    auto releaser = std::async(std::launch::async, [&] {
        waitForWaiters(pool, 1);
        held.release();
    });

    auto conn = pool->acquire(2000ms);
    CHECK(conn->id == held_id);
    CHECK(stats.opened == 1);
    releaser.get();
}

TEST_CASE("Waiters are served in order", "[pool]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.max_size = 1;
    auto pool = makePool(stats, options);

    auto held = pool->acquire();

    std::mutex order_mutex;
    std::vector<char> order;
    auto waiter = [&](char name) {
        auto conn = pool->acquire(5000ms);
        std::lock_guard lock(order_mutex);
        order.push_back(name);
    };

    auto a = std::async(std::launch::async, waiter, 'A');
    waitForWaiters(pool, 1);
    auto b = std::async(std::launch::async, waiter, 'B');
    waitForWaiters(pool, 2);
    auto c = std::async(std::launch::async, waiter, 'C');
    waitForWaiters(pool, 3);

    held.release();
    a.get(); b.get(); c.get();

    CHECK(order == std::vector<char>{ 'A', 'B', 'C' });
}

TEST_CASE("discard() closes the connection and frees its slot", "[pool]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.max_size = 1;
    auto pool = makePool(stats, options);

    SECTION("nobody waiting")
    {
        auto conn = pool->acquire();
        conn.discard();
        CHECK(stats.closed == 1);
        CHECK(pool->size() == 0);
        CHECK(pool->acquire()->id == 2);
    }

    SECTION("a waiter opens a new connection in the freed slot")
    {
        auto conn = pool->acquire();
        auto discarder = std::async(std::launch::async, [&] {
            waitForWaiters(pool, 1);
            conn.discard();
        });

        auto fresh = pool->acquire(2000ms);
        CHECK(fresh->id == 2);
        CHECK(pool->size() == 1);
        discarder.get();
    }
}

TEST_CASE("A failing factory propagates its exception and frees the slot", "[pool][errors]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.max_size = 1;
    auto pool = makePool(stats, options);

    SECTION("direct")
    {
        stats.fail_next_open = true;
        CHECK_THROWS_WITH(pool->acquire(), "database down");
        CHECK(pool->size() == 0);
        CHECK(pool->acquire()->id == 1);    // the database is back
    }

    SECTION("in a slot passed on to a waiter")
    {
        auto conn = pool->acquire();
        auto discarder = std::async(std::launch::async, [&] {
            waitForWaiters(pool, 1);
            stats.fail_next_open = true;
            conn.discard();
        });

        CHECK_THROWS_WITH(pool->acquire(2000ms), "database down");
        discarder.get();
        CHECK(pool->size() == 0);
        CHECK(pool->acquire()->id == 2);
    }
}

TEST_CASE("Idle connections are validated before reuse", "[pool][validate]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.validate_after_idle = 0ms;
    auto pool = makePool(stats, options, true);

    pool->acquire().release();

    SECTION("still valid: reused")
    {
        std::this_thread::sleep_for(2ms);
        CHECK(pool->acquire()->id == 1);
        CHECK(stats.validations == 1);
    }

    SECTION("broken: closed and replaced")
    {
        stats.valid = false;
        std::this_thread::sleep_for(2ms);
        auto conn = pool->acquire();
        CHECK(conn->id == 2);
        CHECK(stats.closed == 1);
        CHECK(pool->size() == 1);
    }

    SECTION("a connection used recently is not validated")
    {
        ConnectionPoolOptions lazy;
        lazy.validate_after_idle = 10s;
        Stats s2;
        auto p2 = makePool(s2, lazy, true);
        p2->acquire().release();
        p2->acquire().release();
        CHECK(s2.validations == 0);
    }
}

TEST_CASE("Connections idle longer than max_idle are closed, down to min_size", "[pool][idle]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.min_size = 1;
    options.max_idle = 20ms;
    auto pool = makePool(stats, options);

    pool->warmUp();
    CHECK(stats.opened == 1);

    {
        auto a = pool->acquire();
        auto b = pool->acquire();
        CHECK(pool->size() == 2);
    }

    std::this_thread::sleep_for(40ms);

    // Two idle and expired: one is closed (above min_size), the other kept
    auto conn = pool->acquire();
    CHECK(stats.closed == 1);
    CHECK(pool->size() == 1);
}

TEST_CASE("With idle checks off, idle connections are never closed", "[pool][idle]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.max_idle = ConnectionPoolOptions::never;
    auto pool = makePool(stats, options);

    pool->acquire().release();
    std::this_thread::sleep_for(20ms);
    CHECK(pool->acquire()->id == 1);
    CHECK(stats.closed == 0);
}

TEST_CASE("warmUp() opens min_size connections, and fails fast", "[pool]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.min_size = 3;
    auto pool = makePool(stats, options);

    pool->warmUp();
    CHECK(pool->size() == 3);
    CHECK(pool->idle() == 3);

    Stats down;
    auto broken = makePool(down, options);
    down.fail_next_open = true;
    CHECK_THROWS_WITH(broken->warmUp(), "database down");
    CHECK(broken->size() == 0);
}

TEST_CASE("close() wakes waiters and closes connections", "[pool][close]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.max_size = 2;
    auto pool = makePool(stats, options);

    auto in_use = pool->acquire();
    pool->acquire().release();          // one idle
    auto other = pool->acquire();       // now both in use... take the idle one back
    other.release();
    auto held = pool->acquire();        // both in use again

    auto waiter = std::async(std::launch::async, [&] { return pool->acquire(5000ms); });
    waitForWaiters(pool, 1);

    pool->close();

    CHECK_THROWS_AS(waiter.get(), gtvr::ConnectionPoolClosed);
    CHECK_THROWS_AS(pool->acquire(), gtvr::ConnectionPoolClosed);

    // In-use connections are closed when they come back
    int closed_before = stats.closed;
    in_use.release();
    held.release();
    CHECK(stats.closed == closed_before + 2);
    CHECK(pool->size() == 0);
    CHECK(stats.alive() == 0);
}

TEST_CASE("Handles are move-only and return the connection once", "[pool]")
{
    Stats stats;
    auto pool = makePool(stats);

    auto a = pool->acquire();
    gtvr::Pooled<FakeConnection> b = std::move(a);
    CHECK_FALSE(a);
    CHECK(b);

    a = pool->acquire();                // assigning over an empty handle
    b = std::move(a);                   // the old b goes back to the pool
    CHECK(pool->idle() == 1);

    b.release();
    CHECK(pool->idle() == 2);
    CHECK(stats.opened == 2);
}

TEST_CASE("Never more than max_size connections, under concurrency", "[pool][threads]")
{
    Stats stats;
    ConnectionPoolOptions options;
    options.max_size = 4;
    options.acquire_timeout = 10s;
    auto pool = makePool(stats, options);

    constexpr int threads = 8;
    constexpr int rounds = 3000;
    std::atomic<int> timeouts{ 0 };

    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t)
    {
        workers.emplace_back([&] {
            for (int i = 0; i < rounds; ++i)
            {
                try {
                    auto conn = pool->acquire();
                    int now = ++stats.in_use;
                    int prev = stats.max_in_use;
                    while (now > prev && !stats.max_in_use.compare_exchange_weak(prev, now)) {}

                    if (i % 100 == 0)
                        conn.discard();         // some connections break

                    --stats.in_use;
                }
                catch (const gtvr::ConnectionPoolTimeout&) {
                    ++timeouts;
                }
            }
        });
    }

    for (auto& w : workers)
        w.join();

    CHECK(timeouts == 0);
    CHECK(stats.max_in_use <= 4);
    CHECK(pool->size() <= 4);
    CHECK(stats.alive() == static_cast<int>(pool->size()));

    pool->close();
    CHECK(stats.alive() == 0);
}

TEST_CASE("Options are checked", "[pool]")
{
    Stats stats;
    ConnectionPoolOptions bad;
    bad.max_size = 0;
    CHECK_THROWS_AS(makePool(stats, bad), std::invalid_argument);

    bad.max_size = 2;
    bad.min_size = 3;
    CHECK_THROWS_AS(makePool(stats, bad), std::invalid_argument);

    CHECK_THROWS_AS(ConnectionPool<FakeConnection>::create({}, nullptr), std::invalid_argument);
}
