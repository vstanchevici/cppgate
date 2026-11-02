#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// Internal: used by the Listener and the sessions for the graceful stop()
namespace gtvr::detail {

    // A live client connection (HttpSession, WebSocketSession) the Listener can ask to stop
    class Session
    {
        public:
            virtual ~Session() = default;

            // Called from any thread: the connection moves to its own strand
            virtual void stop() = 0;
    };

    // The live sessions of a Listener, shared with them (they may outlive it)
    class SessionRegistry
    {
        private:
            std::mutex                                                      mutex_;
            std::unordered_map<Session*, std::weak_ptr<Session>>           items_;
            std::atomic<bool>                                               stopping_{ false };

        public:
            // False when stopping: the session must close instead of starting
            bool add(const std::shared_ptr<Session>& session)
            {
                std::lock_guard lock(mutex_);

                if (stopping_)
                    return false;

                items_[session.get()] = session;
                return true;
            }

            // From the session's destructor
            void remove(Session* session)
            {
                std::lock_guard lock(mutex_);
                items_.erase(session);
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
                std::vector<std::shared_ptr<Session>> alive;

                {
                    std::lock_guard lock(mutex_);
                    stopping_ = true;

                    for (auto& [ptr, weak] : items_)
                        if (auto session = weak.lock())
                            alive.push_back(std::move(session));
                }

                // Outside the lock: releasing the last reference runs a destructor, which calls remove()
                for (auto& session : alive)
                    session->stop();
            }
    };

}//namespace gtvr::detail
