#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gtvr {

    namespace router { class Router; }

    // A received message: a view over the read buffer, only valid during the onMessage callback.
    // Copy what you need to keep (e.g. std::string(msg.text())).
    class WebSocketMessage
    {
        private:
            std::string_view    data_;
            bool                is_text_;

        public:
            WebSocketMessage(std::string_view data, bool is_text) noexcept : data_(data), is_text_(is_text) {}

            bool isText() const noexcept   { return is_text_; }
            bool isBinary() const noexcept { return !is_text_; }

            std::string_view text() const noexcept { return data_; }

            std::span<const uint8_t> bytes() const noexcept
            {
                return { reinterpret_cast<const uint8_t*>(data_.data()), data_.size() };
            }

            size_t size() const noexcept { return data_.size(); }
            bool empty() const noexcept  { return data_.empty(); }
    };

    /*
        One WebSocket connection, as seen by route handlers and middlewares.

        Lifecycle:
            1. Pre-middlewares run BEFORE the handshake. Return false (optionally after reject())
               to refuse the upgrade with an HTTP response.
            2. The handshake is accepted, then the route handler runs: this is the onOpen event.
               Set the callbacks here, or call close().
            3. Messages are read only after the handler returns, so no message is lost.

        Guarantees:
            - All callbacks of one connection run on its strand, never concurrently.
            - onClose is called exactly once after the connection was opened, whatever closed it.
            - The WebSocketMessage passed to onMessage is only valid during the callback.
            - send*() and close() are safe to call from any thread.

        Callbacks receive the context as a parameter: do not capture the shared_ptr in them,
        the connection owns its callbacks and that would be a reference cycle.
    */
    class WebSocketContext
    {
        public:
            using Request         = boost::beast::http::request<boost::beast::http::string_body>;
            using CloseCode       = boost::beast::websocket::close_code;

            using MessageCallback = std::function<void(WebSocketContext&, const WebSocketMessage& message)>;
            using CloseCallback   = std::function<void(WebSocketContext&, CloseCode code)>;
            using ErrorCallback   = std::function<void(WebSocketContext&, boost::beast::error_code ec)>;

        private:
            MessageCallback                                     on_message_;
            CloseCallback                                       on_close_;
            ErrorCallback                                       on_error_;

            std::shared_ptr<void>                               context_;
            std::string                                         ip_;
            std::string                                         peer_ip_;
            std::vector<std::pair<std::string, std::string>>    params_;

            boost::beast::http::status                          reject_status_ = boost::beast::http::status::unknown;
            std::string                                         reject_body_;

            static constexpr size_t                             no_route = std::numeric_limits<size_t>::max();
            size_t                                              route_index_ = no_route;

            bool                                                closed_ = false;

            friend router::Router;

        protected:
            explicit WebSocketContext(std::string peer_ip) : ip_(peer_ip), peer_ip_(std::move(peer_ip)) {}

        public:
            virtual ~WebSocketContext() = default;

            // Client IP: the socket address, or the one forwarded by a trusted proxy (see Router::setTrustedProxies)
            const std::string& ip() const noexcept { return ip_; }

            // The address of the socket peer (the proxy when behind one)
            const std::string& peerIP() const noexcept { return peer_ip_; }

            // --- Callbacks, set them in the route handler ---
            void onMessage(MessageCallback callback) { on_message_ = std::move(callback); }
            void onClose(CloseCallback callback)     { on_close_ = std::move(callback); }
            void onError(ErrorCallback callback)     { on_error_ = std::move(callback); }

            // --- User data, e.g. set by an auth middleware and read later in the callbacks ---
            void setContext(std::shared_ptr<void> ctx) noexcept {
                context_ = std::move(ctx);
            }

            template<typename T>
            std::shared_ptr<T> getContext() const noexcept {
                return std::static_pointer_cast<T>(context_);
            }

            // --- Path parameters of the matched route, e.g. {room} in /ws/rooms/{room} ---
            std::string_view urlParam(std::string_view key) const
            {
                for (const auto& [k, v] : params_) {
                    if (k == key) return v;
                }
                return {};
            }

            // Only meaningful in a pre-middleware that returns false. Defaults to 403 Forbidden.
            void reject(boost::beast::http::status status, std::string body = {})
            {
                reject_status_ = status;
                reject_body_ = std::move(body);
            }

            // --- Implemented by the session ---
            virtual const Request& request() const = 0;

            virtual void sendText(std::string data) = 0;

            virtual void sendText(std::shared_ptr<const std::string> data) = 0;

            virtual void sendBinary(std::vector<uint8_t> data) = 0;

            virtual void close(CloseCode code = CloseCode::normal) = 0;

        protected:
            // Called by the session, on its strand
            void emitMessage(std::string_view data, bool is_text)
            {
                if (on_message_ && !closed_)
                    on_message_(*this, WebSocketMessage(data, is_text));
            }

            void emitError(boost::beast::error_code ec)
            {
                if (on_error_ && !closed_)
                    on_error_(*this, ec);
            }

            void emitClose(CloseCode code)
            {
                if (closed_)
                    return;

                closed_ = true;

                // Release the user's callbacks once the connection is done
                auto callback = std::move(on_close_);
                on_message_ = nullptr;
                on_error_ = nullptr;

                if (callback)
                    callback(*this, code);
            }
    };

}//namespace gtvr
