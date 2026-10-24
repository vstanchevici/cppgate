#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gtvr {

    // A live connection the Listener can ask to stop, for a graceful shutdown
    class Connection
    {
        public:
            virtual ~Connection() = default;

            // Called from any thread: the connection moves to its own strand
            virtual void stop() = 0;
    };

    // The live connections of a Listener, shared with its sessions (which may outlive it)
    class Connections
    {
        private:
            std::mutex                                                      mutex_;
            std::unordered_map<Connection*, std::weak_ptr<Connection>>     items_;
            std::atomic<bool>                                               stopping_{ false };

        public:
            // False when stopping: the connection must close instead of starting
            bool add(const std::shared_ptr<Connection>& connection)
            {
                std::lock_guard lock(mutex_);

                if (stopping_)
                    return false;

                items_[connection.get()] = connection;
                return true;
            }

            // From the connection's destructor
            void remove(Connection* connection)
            {
                std::lock_guard lock(mutex_);
                items_.erase(connection);
            }

            bool stopping() const noexcept
            {
                return stopping_;
            }

            std::size_t size()
            {
                std::lock_guard lock(mutex_);
                return items_.size();
            }

            void stopAll()
            {
                std::vector<std::shared_ptr<Connection>> alive;

                {
                    std::lock_guard lock(mutex_);
                    stopping_ = true;

                    for (auto& [ptr, weak] : items_)
                        if (auto connection = weak.lock())
                            alive.push_back(std::move(connection));
                }

                // Outside the lock: releasing the last reference runs a destructor, which calls remove()
                for (auto& connection : alive)
                    connection->stop();
            }
    };

}//namespace gtvr
