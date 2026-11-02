#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gtvr {

    struct ConnectionPoolOptions
    {
        // For max_idle: never close idle connections. With no validate function either, the pool
        // doesn't need to read the clock at all (about 100 ns less per acquire + release).
        static constexpr std::chrono::milliseconds never = std::chrono::milliseconds::max();

        // Created by warmUp() and never closed for being idle
        std::size_t                     min_size            = 0;

        // Connections open at the same time, in use or idle
        std::size_t                     max_size            = 10;

        // How long acquire() waits for a free connection when max_size are in use
        std::chrono::milliseconds       acquire_timeout     { 5000 };

        // An idle connection is checked with the validate function before being handed out
        // when it has been idle longer than this (a database may drop idle connections)
        std::chrono::milliseconds       validate_after_idle { 30000 };

        // Idle connections above min_size are closed after this long
        std::chrono::milliseconds       max_idle            { 300000 };
    };

    // acquire() found no free connection within the timeout
    class ConnectionPoolTimeout : public std::runtime_error
    {
        public:
            ConnectionPoolTimeout() : std::runtime_error("ConnectionPool: no connection available within the timeout") {}
    };

    // acquire() after close()
    class ConnectionPoolClosed : public std::runtime_error
    {
        public:
            ConnectionPoolClosed() : std::runtime_error("ConnectionPool: closed") {}
    };

    template<typename T>
    class ConnectionPool;

    /*
        A connection borrowed from the pool. It goes back to the pool when this handle is destroyed
        (or on release()). Call discard() after an error that may have broken the connection: it is
        closed instead, and the pool opens a new one when needed. Move-only.
    */
    template<typename T>
    class Pooled
    {
        private:
            using Clock = std::chrono::steady_clock;

            std::shared_ptr<ConnectionPool<T>>  pool_;
            std::optional<T>                    connection_;

            friend ConnectionPool<T>;

            Pooled(std::shared_ptr<ConnectionPool<T>> pool, T connection)
                : pool_(std::move(pool)), connection_(std::move(connection)) {}

        public:
            Pooled() = default;

            // T only needs to be move-constructible. The source is emptied explicitly: a moved-from
            // std::optional would still hold a (moved-from) value.
            Pooled(Pooled&& other) noexcept
                : pool_(std::move(other.pool_))
            {
                if (other.connection_)
                    connection_.emplace(std::move(*other.connection_));
                other.connection_.reset();
            }

            Pooled& operator=(Pooled&& other) noexcept
            {
                if (this != &other)
                {
                    release();
                    pool_ = std::move(other.pool_);
                    if (other.connection_)
                        connection_.emplace(std::move(*other.connection_));
                    other.connection_.reset();
                }
                return *this;
            }

            Pooled(const Pooled&) = delete;
            Pooled& operator=(const Pooled&) = delete;

            ~Pooled()
            {
                release();
            }

            T& get()                    { return *connection_; }
            const T& get() const        { return *connection_; }
            T& operator*()              { return *connection_; }
            T* operator->()             { return &*connection_; }

            explicit operator bool() const noexcept { return connection_.has_value(); }

            // Back to the pool now
            void release()
            {
                if (connection_ && pool_)
                    pool_->giveBack(std::move(*connection_));
                connection_.reset();
                pool_.reset();
            }

            // Close it instead of giving it back
            void discard()
            {
                if (connection_ && pool_)
                {
                    connection_.reset();        // the connection's destructor closes it, outside the pool's lock
                    pool_->dropped();
                }
                connection_.reset();
                pool_.reset();
            }
    };

    /*
        A pool of database (or any other) connections, safe to use from any thread.

            auto pool = gtvr::ConnectionPool<Connection>::create(options,
                [] { return connect(...); },                // opens one, throws on failure
                [](Connection& c) { return c.ping(); });    // optional: still usable?

            auto conn = pool->acquire();    // waits up to acquire_timeout, throws ConnectionPoolTimeout
            conn->query(...);               // back to the pool at the end of the scope

        T is whatever the driver gives: a connection object, a std::unique_ptr or a shared handle;
        it only has to be move-constructible. The factory and validate function are called from
        several threads at once, they must not share unprotected state. acquire() blocks the calling thread while it waits, never longer
        than the timeout, and while a new connection is opened.

        Waiters are served in order (FIFO). The most recently used idle connection is handed out first.
        The factory and validate function run outside the pool's lock, so a slow connect doesn't stop
        other threads from getting and returning connections.
    */
    template<typename T>
    class ConnectionPool : public std::enable_shared_from_this<ConnectionPool<T>>
    {
        public:
            using Factory   = std::function<T()>;
            using Validator = std::function<bool(T&)>;

        private:
            using Clock = std::chrono::steady_clock;

            struct Idle
            {
                T                   connection;
                Clock::time_point   since;
            };

            // A thread waiting in acquire(): served in order by giveBack() / dropped()
            struct Waiter
            {
                std::condition_variable cv;
                std::optional<T>        connection;         // handed over directly
                bool                    may_open = false;   // a slot was freed: open a new one
                bool                    closed = false;
            };

            ConnectionPoolOptions       options_;
            Factory                     factory_;
            Validator                   validate_;
            bool                        timed_;             // idle checks on: connections carry a timestamp

            std::mutex                  mutex_;
            std::vector<Idle>           idle_;              // back = most recently used
            std::deque<Waiter*>         waiters_;
            std::size_t                 open_ = 0;          // idle + in use + being opened
            bool                        closed_ = false;

            friend Pooled<T>;

            struct Private {};

        public:
            ConnectionPool(Private, ConnectionPoolOptions options, Factory factory, Validator validate)
                : options_(options), factory_(std::move(factory)), validate_(std::move(validate)),
                  timed_(validate_ || options_.max_idle != ConnectionPoolOptions::never)
            {
                if (!factory_)
                    throw std::invalid_argument("ConnectionPool: a factory is required");
                if (options_.max_size == 0 || options_.min_size > options_.max_size)
                    throw std::invalid_argument("ConnectionPool: needs 0 <= min_size <= max_size and max_size > 0");
            }

            // Handles keep the pool alive, so it is always owned by a shared_ptr
            static std::shared_ptr<ConnectionPool> create(ConnectionPoolOptions options, Factory factory, Validator validate = {})
            {
                return std::make_shared<ConnectionPool>(Private{}, options, std::move(factory), std::move(validate));
            }

            ~ConnectionPool()
            {
                close();
            }

            // Opens connections up to min_size now, e.g. at startup to fail fast if the database is down.
            // Throws what the factory throws.
            void warmUp()
            {
                for (;;)
                {
                    {
                        std::lock_guard lock(mutex_);
                        if (closed_ || open_ >= options_.min_size)
                            return;
                        ++open_;
                    }

                    giveBack(openReserved());
                }
            }

            Pooled<T> acquire()
            {
                return acquire(options_.acquire_timeout);
            }

            // Throws ConnectionPoolTimeout, ConnectionPoolClosed, or what the factory throws.
            // A timeout of 0 doesn't wait for another thread (it may still open a connection).
            Pooled<T> acquire(std::chrono::milliseconds timeout)
            {
                // Only read the clock if we have to wait
                std::optional<Clock::time_point> deadline;

                for (;;)
                {
                    std::unique_lock lock(mutex_);

                    if (closed_)
                        throw ConnectionPoolClosed();

                    // 1. An idle connection
                    if (!idle_.empty())
                    {
                        Idle idle = std::move(idle_.back());
                        idle_.pop_back();
                        bool above_min = open_ > options_.min_size;
                        lock.unlock();

                        if (auto usable = checkOut(std::move(idle), above_min))
                            return Pooled<T>(this->shared_from_this(), std::move(*usable));

                        continue;   // it was closed, its slot freed: try again
                    }

                    // 2. Room for a new one
                    if (open_ < options_.max_size)
                    {
                        ++open_;
                        lock.unlock();
                        return Pooled<T>(this->shared_from_this(), openReserved());
                    }

                    // 3. Wait for one to come back, in order
                    if (!deadline)
                        deadline = Clock::now() + timeout;

                    Waiter waiter;
                    waiters_.push_back(&waiter);

                    bool served = waiter.cv.wait_until(lock, *deadline, [&] {
                        return waiter.connection.has_value() || waiter.may_open || waiter.closed;
                    });

                    if (!served)
                    {
                        waiters_.erase(std::find(waiters_.begin(), waiters_.end(), &waiter));
                        throw ConnectionPoolTimeout();
                    }

                    if (waiter.closed)
                        throw ConnectionPoolClosed();

                    lock.unlock();

                    // Handed over by giveBack(): it was just used, no need to validate it
                    if (waiter.connection)
                        return Pooled<T>(this->shared_from_this(), std::move(*waiter.connection));

                    // dropped() passed its slot on to us
                    return Pooled<T>(this->shared_from_this(), openReserved());
                }
            }

            // Closes the idle connections, wakes the waiting threads (they throw ConnectionPoolClosed).
            // Connections in use are closed when their handle is released.
            void close()
            {
                std::vector<Idle> to_close;

                {
                    std::lock_guard lock(mutex_);
                    if (closed_)
                        return;

                    closed_ = true;
                    to_close.swap(idle_);
                    open_ -= to_close.size();

                    for (auto* waiter : waiters_)
                    {
                        waiter->closed = true;
                        waiter->cv.notify_one();
                    }
                    waiters_.clear();
                }
                // to_close goes out of scope here: the connections are closed outside the lock
            }

            // Open connections (idle + in use + being opened)
            std::size_t size()
            {
                std::lock_guard lock(mutex_);
                return open_;
            }

            std::size_t idle()
            {
                std::lock_guard lock(mutex_);
                return idle_.size();
            }

            // Threads waiting in acquire()
            std::size_t waiting()
            {
                std::lock_guard lock(mutex_);
                return waiters_.size();
            }

        private:
            // Opens a connection for a slot already counted in open_; frees the slot if it fails
            T openReserved()
            {
                try {
                    return factory_();
                }
                catch (...) {
                    dropped();
                    throw;
                }
            }

            // An idle connection about to be handed out: too old, or broken? Then close it.
            std::optional<T> checkOut(Idle idle, bool above_min)
            {
                if (!timed_)
                    return std::move(idle.connection);

                auto idle_for = Clock::now() - idle.since;

                bool expired = idle_for > options_.max_idle && above_min;
                bool broken = !expired && validate_ && idle_for > options_.validate_after_idle && !isValid(idle.connection);

                if (!expired && !broken)
                    return std::move(idle.connection);

                {
                    Idle closing = std::move(idle);     // closed here, outside the lock
                }
                dropped();
                return std::nullopt;
            }

            bool isValid(T& connection)
            {
                try {
                    return validate_(connection);
                }
                catch (...) {
                    return false;
                }
            }

            // From a Pooled handle: hand it to the first waiter, or keep it idle
            void giveBack(T connection)
            {
                // Read before taking the lock, it's the slowest part
                auto now = timed_ ? Clock::now() : Clock::time_point{};

                std::unique_lock lock(mutex_);

                if (closed_)
                {
                    --open_;
                    lock.unlock();
                    return;     // `connection` is closed here, outside the lock
                }

                if (!waiters_.empty())
                {
                    Waiter* waiter = waiters_.front();
                    waiters_.pop_front();
                    waiter->connection.emplace(std::move(connection));
                    waiter->cv.notify_one();
                    return;
                }

                idle_.push_back({ std::move(connection), now });
            }

            // A connection was closed (discarded, expired, broken, or failed to open): pass its slot on
            void dropped()
            {
                std::lock_guard lock(mutex_);

                if (!closed_ && !waiters_.empty())
                {
                    // The slot stays counted in open_, the waiter opens a connection in it
                    Waiter* waiter = waiters_.front();
                    waiters_.pop_front();
                    waiter->may_open = true;
                    waiter->cv.notify_one();
                    return;
                }

                --open_;
            }
    };

}//namespace gtvr
