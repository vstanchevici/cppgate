#pragma once

#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <cppgate/Log.h>
#include <cppgate/Router.h>
#include <cppgate/ServerOptions.h>
#include <cppgate/SessionRegistry.h>
#include <cppgate/detail/TlsStream.h>
#include <cppgate/WebSocketContext.h>
#include <atomic>
#include <functional>
#include <deque>
#include <memory>

namespace gtvr {

// Define stream types
using ws_plain_t = boost::beast::websocket::stream<boost::beast::tcp_stream>;
using ws_ssl_t  = boost::beast::websocket::stream<detail::TlsStream>;

// Declare the type ws_stream_t
template<bool with_plain, bool with_ssl>
using ws_stream_t = std::conditional_t<
        with_plain && with_ssl,
        std::variant<ws_plain_t, ws_ssl_t>, // Both
        std::conditional_t<
            with_plain,
            std::variant<ws_plain_t>,      // Plain only
            std::variant<ws_ssl_t>         // SSL only
        >
>;

template<bool with_plain, bool with_ssl>
class WebSocketSession: public WebSocketContext, public detail::Session, public std::enable_shared_from_this<WebSocketSession<with_plain, with_ssl>>
{
    static_assert(with_plain || with_ssl, "WebSocketSession must have or plain or ssl stream.");

    using std::enable_shared_from_this<WebSocketSession<with_plain, with_ssl>>::shared_from_this;

    struct ws_message {
        // We use const to ensure thread-safety across multiple sessions
        std::shared_ptr<const std::string> data;
        bool is_text;

        ws_message(std::shared_ptr<const std::string> d, bool t)
            : data(std::move(d)), is_text(t) {}
    };

    private:
        std::deque<ws_message>                                          write_queue_;

        // Sent but not written yet, counted from the send*() call so any thread can read it
        std::atomic<std::size_t>                                        pending_{ 0 };

        boost::beast::flat_buffer                                       buffer_;
        Request                                                         request_;
        boost::beast::http::response<boost::beast::http::string_body>   reject_response_;
        ws_stream_t<with_plain, with_ssl>                               stream_;
        ServerOptions                                                   options_;
        std::shared_ptr<detail::SessionRegistry>                    sessions_;
        bool                                                            closing_ = false;

        // Runs a user callback, an exception must not escape into the io_context
        template<typename F>
        bool guarded(const char* what, F&& fn)
        {
            try {
                fn();
                return true;
            }
            catch (const std::exception& e) {
                slog_error("{} threw: {}", what, e.what());
            }
            catch (...) {
                slog_error("{} threw an unknown exception", what);
            }
            return false;
        }

        // Refuse the upgrade with a plain HTTP response, then close the connection
        void do_reject(gtvr::router::HttpResponse response)
        {
            if (!response.hasResponse())
                response.send(boost::beast::http::status::internal_server_error, "Internal Server Error");

            reject_response_ = std::move(response.beast_res_);
            reject_response_.version(request_.version());
            reject_response_.keep_alive(false);
            reject_response_.prepare_payload();

            std::visit([this](auto& ws)
            {
                boost::beast::get_lowest_layer(ws).expires_after(options_.http_timeout);
                boost::beast::http::async_write(ws.next_layer(), reject_response_, boost::beast::bind_front_handler(&WebSocketSession::on_reject, shared_from_this()));
            }, stream_);
        }

        void on_reject(boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);

            if (ec)
                slog_debug("reject: {}", ec.message());

            std::visit([this](auto& ws)
            {
                using T = std::decay_t<decltype(ws)>;

                if constexpr (std::is_same_v<T, ws_ssl_t>)
                {
                    ws.next_layer().async_shutdown(boost::beast::bind_front_handler(&WebSocketSession::on_reject_shutdown, this->shared_from_this()));
                }
                else
                {
                    boost::beast::error_code ec_ignore;
                    ws.next_layer().socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec_ignore);
                }
            }, stream_);
        }

        void on_reject_shutdown(boost::beast::error_code ec)
        {
            // Nothing left to do, the socket closes when the session is destroyed
            boost::ignore_unused(ec);
        }

        void do_accept(std::shared_ptr<gtvr::router::Router> router)
        {
            boost::beast::websocket::stream_base::timeout opt;
            opt.handshake_timeout = options_.websocket_handshake_timeout;   // also used for the close handshake
            opt.idle_timeout = options_.websocket_idle_timeout;
            opt.keep_alive_pings = options_.websocket_keep_alive_pings;

            // Apply the options to the stream using your visit pattern
            std::visit([&opt, router, this](auto& ws)
            {
                // Set maximum read message
                ws.read_message_max(options_.websocket_message_limit);

                //Set timeout options
                ws.set_option(opt);

                // Set a decorator to change the server name
                ws.set_option(boost::beast::websocket::stream_base::decorator([](boost::beast::websocket::response_type& res)
                 {
                     res.set(boost::beast::http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " CppGate Server");
                 }));

                // Accept the websocket handshake
                ws.async_accept(request_, boost::beast::bind_front_handler(&WebSocketSession::on_accept, shared_from_this(), router));
            }, stream_);
        }

        void on_accept(std::shared_ptr<gtvr::router::Router> router, boost::beast::error_code ec)
        {
            if (ec)
            {
                slog_debug("accept: {}", ec.message());
                return;
            }

            // The Listener started stopping during the handshake: don't open, say goodbye
            if (sessions_ && sessions_->stopping())
            {
                close(CloseCode::going_away);
                do_read();
                return;
            }

            // onOpen event: the route handler sets the callbacks or closes
            router->open(shared_from_this());

            // Read only after the handler, so no message arrives before onMessage is set
            do_read();
        }

        void do_read()
        {
            // Read a message into our buffer
            std::visit([this](auto& ws)
            {
                ws.async_read(buffer_, boost::beast::bind_front_handler(&WebSocketSession::on_read, shared_from_this()));
            }, stream_);
        }

        void on_read(boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);

            // This indicates that the websocket_session was closed
            if (ec == boost::beast::websocket::error::closed)
            {
                auto code = std::visit([](auto& ws) { return CloseCode(ws.reason().code); }, stream_);
                guarded("onClose", [&] { emitClose(code); });
                return;
            }

            if (ec)
            {
                if (ec != boost::beast::net::error::operation_aborted)
                {
                    // Usually the client disconnecting: the application decides in onError if it matters
                    slog_debug("read: {}", ec.message());
                    guarded("onError", [&] { emitError(ec); });
                }

                guarded("onClose", [&] { emitClose(CloseCode::abnormal); });
                return;
            }

            bool is_text = std::visit([](auto& ws) { return ws.got_text(); }, stream_);
            std::string_view data(static_cast<const char*>(buffer_.data().data()), buffer_.size());

            if (!guarded("onMessage", [&] { emitMessage(data, is_text); }))
                close(CloseCode::internal_error);

            // Clear the buffer
            buffer_.consume(buffer_.size());

            // Keep reading even when closing: the read completes the close handshake
            do_read();
        }

        void do_write()
        {
            if (!write_queue_.empty())
            {
                auto& data = write_queue_.front();

                std::visit([this, data](auto& ws) {
                    ws.text(data.is_text);
                    ws.async_write(boost::beast::net::buffer(*data.data), boost::beast::bind_front_handler(&WebSocketSession::on_write, shared_from_this()));
                }, stream_);
            }
        }

        void on_write(boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);

            if (ec)
            {
                // The read loop reports the failure to the user
                if (ec != boost::beast::net::error::operation_aborted)
                    slog_debug("write: {}", ec.message());

                pending_ -= write_queue_.size();
                write_queue_.clear();
                return;
            }

            write_queue_.pop_front();
            --pending_;

            do_write();
        }

        void on_close(CloseCode code, boost::beast::error_code ec)
        {
            if (ec && ec != boost::beast::net::error::operation_aborted)
            {
                // This is common if the TCP connection dropped
                // before the WebSocket close handshake finished.
                slog_debug("close: {}", ec.message());
            }

            // Usually the read loop already reported it, emitClose ignores the second call
            guarded("onClose", [&] { emitClose(code); });
        }

        inline boost::beast::net::any_io_executor get_strand()
        {
            return std::visit([](auto& ws) { return boost::beast::net::any_io_executor(ws.get_executor()); }, stream_);
        }

        void send_internal(std::shared_ptr<const std::string> msg, bool is_text)
        {
            ++pending_;

            // We must move to the strand to protect the queue
            boost::beast::net::post(get_strand(), [this, self = shared_from_this(), msg = std::move(msg), is_text]() mutable {

                if (closing_)
                {
                    --pending_;
                    return;
                }

                // Slow consumer: it doesn't read as fast as we send. Waiting would grow memory without
                // limit and silently dropping would corrupt what the application thinks was delivered.
                if (write_queue_.size() >= options_.websocket_send_queue_limit)
                {
                    --pending_;
                    slog_debug("websocket {}: send queue full ({} messages), closing", ip(), write_queue_.size());
                    close(CloseCode::policy_error);
                    return;
                }

                // Add to queue
                write_queue_.emplace_back(std::move(msg), is_text);

                // Start the write loop if idle
                if (write_queue_.size() == 1) {
                    do_write();
                }
            });
        }

    public:

        WebSocketSession(boost::beast::tcp_stream stream, Request request, std::string peer_ip, const ServerOptions& options, std::shared_ptr<detail::SessionRegistry> sessions = {}) requires (with_plain)
            : WebSocketContext(std::move(peer_ip)), request_(std::move(request)), stream_(ws_plain_t(std::move(stream))), options_(options), sessions_(std::move(sessions))
        {
        }

        WebSocketSession(detail::TlsStream stream, Request request, std::string peer_ip, const ServerOptions& options, std::shared_ptr<detail::SessionRegistry> sessions = {}) requires (with_ssl)
            : WebSocketContext(std::move(peer_ip)), request_(std::move(request)), stream_(ws_ssl_t(std::move(stream))), options_(options), sessions_(std::move(sessions))
        {
        }

        // Must be called on the connection's strand
        ~WebSocketSession()
        {
            if (sessions_)
                sessions_->remove(this);
        }

        // Graceful shutdown, from any thread: close with 1001 (going away)
        void stop() override
        {
            close(CloseCode::going_away);
        }

        void run(std::shared_ptr<gtvr::router::Router> router)
        {
            gtvr::router::HttpResponse response;

            // Upgrade requested while the Listener is stopping
            if (sessions_ && !sessions_->add(shared_from_this()))
            {
                response.send(boost::beast::http::status::service_unavailable, "Server is shutting down");
                return do_reject(std::move(response));
            }

            if (router->upgrade(shared_from_this(), response))
                do_accept(router);
            else
                do_reject(std::move(response));
        }

        const Request& request() const override
        {
            return request_;
        }

        void close(CloseCode code = CloseCode::normal) override
        {
            boost::beast::net::post(get_strand(), [this, self = shared_from_this(), code]() {
                std::visit([this, code](auto& ws) {
                    if (closing_ || !ws.is_open())
                        return;

                    closing_ = true;

                    ws.async_close(code, boost::beast::bind_front_handler(&WebSocketSession::on_close, shared_from_this(), code));
                }, stream_);
            });
        }

        void sendText(std::shared_ptr<const std::string> data) override
        {
            send_internal(std::move(data), true);
        }

        void sendText(std::string data) override
        {
            send_internal(std::make_shared<std::string>(std::move(data)), true);
        }

        void sendBinary(std::vector<uint8_t> data) override
        {
            send_internal(std::make_shared<std::string>(data.begin(), data.end()), false);
        }

        std::size_t pendingMessages() const override
        {
            return pending_;
        }
};

}//namespace gtvr
