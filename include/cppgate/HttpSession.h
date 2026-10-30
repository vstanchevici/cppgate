#pragma once

#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include "WebSocketSession.h"
#include <cppgate/Router.h>
#include <cppgate/ServerOptions.h>
#include <cppgate/SessionRegistry.h>
#include <deque>
#include <optional>
#include <variant>

namespace gtvr {


// Define stream types
using h_plain_t = boost::beast::tcp_stream;
using h_ssl_t  = boost::beast::ssl_stream<boost::beast::tcp_stream>;

// Declare the type http_stream_t
template<bool with_plain, bool with_ssl>
using http_stream_t = std::conditional_t<
        with_plain && with_ssl,
        std::variant<h_plain_t, h_ssl_t>, // Both
        std::conditional_t<
            with_plain,
            std::variant<h_plain_t>,      // Plain only
            std::variant<h_ssl_t>         // SSL only
        >
>;

template<bool with_plain, bool with_ssl>
class HttpSession : public detail::Session, public std::enable_shared_from_this<HttpSession<with_plain, with_ssl>>
{
    static_assert(with_plain || with_ssl, "HttpSession must have or plain or ssl stream.");

    using std::enable_shared_from_this<HttpSession<with_plain, with_ssl>>::shared_from_this;

    private:
        std::deque<boost::beast::http::message_generator>                                       response_queue_;
        boost::optional<boost::beast::http::request_parser<boost::beast::http::string_body>>    parser_;
        boost::beast::flat_buffer                                                               buffer_;
        http_stream_t<with_plain, with_ssl>                                                     stream_;


        ServerOptions                                                                           options_;

        // Set after an error response: close the connection gracefully, see ServerOptions::http_linger_timeout
        bool                                                                                    linger_ = false;
        bool                                                                                    lingering_ = false;
        std::vector<char>                                                                       linger_buffer_;

        // Graceful shutdown: the Listener's session registry, and whether we wait for the client (detect, handshake, read)
        std::shared_ptr<detail::SessionRegistry>                                            sessions_;
        bool                                                                                    waiting_ = false;

        bool stopping() const noexcept
        {
            return sessions_ && sessions_->stopping();
        }
    
    private:
        void queue_write(boost::beast::http::message_generator response, std::shared_ptr<gtvr::router::Router> router)
        {
            // Allocate and store the work
            response_queue_.push_back(std::move(response));

            // If there was no previous work, start the write loop
            if (response_queue_.size() == 1)
                do_write(router);
        }

        // Called to start/continue the write-loop. Should not be called when
        // write_loop is already active.
        //
        // Returns `true` if the caller may initiate a new read
        void do_write(std::shared_ptr<gtvr::router::Router> router)
        {
            if (!response_queue_.empty())
            {
                auto main_executor = std::visit([](auto& hs) { return boost::beast::get_lowest_layer(hs).get_executor(); }, stream_);

                // Create a strong shared reference before posting
                auto self = shared_from_this();

                // Capture 'self' by value to secure the object lifetime
                boost::asio::post(main_executor, [this, self, router]() mutable
                {
                    if (response_queue_.empty()) return;

                    boost::beast::http::message_generator msg = std::move(response_queue_.front());
                    response_queue_.pop_front();

                    const bool keep_alive = msg.keep_alive();

                    std::visit([this, self, msg = std::move(msg), router, keep_alive](auto& hs) mutable
                    {
                        auto stream_executor = boost::beast::get_lowest_layer(hs).get_executor();

                        auto bound_handler = boost::asio::bind_executor(
                            stream_executor,
                            boost::beast::bind_front_handler(&HttpSession::on_write, self, router, keep_alive)
                        );

                        boost::beast::async_write(hs, std::move(msg), std::move(bound_handler));
                    }, stream_);
                });
            }

        }

        void on_write(std::shared_ptr<gtvr::router::Router> router, bool keep_alive, boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);

            if (ec)
            {
                slog_debug("write: {}", ec.message());
                do_eof(); // Clean up socket descriptors and self references safely
                return;
            }
            
            if (response_queue_.empty())
            {
                // While stopping, no new request on this connection
                if (keep_alive && !stopping())
                    do_read(router);
                else
                    do_eof();
            }
            else
                do_write(router);
        }


    public:
        // Starts plain, then detects TLS if with_ssl
        explicit HttpSession(boost::asio::ip::tcp::socket&& socket, const ServerOptions& options, std::shared_ptr<detail::SessionRegistry> sessions = {}) requires (with_plain)
            : stream_(boost::beast::tcp_stream(std::move(socket))), options_(options), sessions_(std::move(sessions))
        {
        }
        
        // SSL only: TLS from the first byte
        explicit HttpSession(boost::asio::ip::tcp::socket&& socket, boost::asio::ssl::context* ctx, const ServerOptions& options, std::shared_ptr<detail::SessionRegistry> sessions = {}) requires (!with_plain)
            : stream_(std::in_place_type<h_ssl_t>, std::move(socket), *ctx), options_(options), sessions_(std::move(sessions))
        {
            
        }

        ~HttpSession()
        {
            if (sessions_)
                sessions_->remove(this);
        }

        // Graceful shutdown, from any thread. A request being read or answered is finished first,
        // a connection waiting for its next request is closed now.
        void stop() override
        {
            boost::beast::net::post(
                std::visit([](auto& s) { return s.get_executor(); }, stream_),
                [self = shared_from_this()]() { self->on_stop(); });
        }
        
    
        // Launch the detector
        void run(boost::asio::ssl::context* ctx, std::shared_ptr<gtvr::router::Router> router)
        {
            // Accepted while the Listener is stopping: returning drops the last reference, the socket closes
            if (sessions_ && !sessions_->add(shared_from_this()))
                return;

            // We need to be executing within a strand to perform async operations
            // on the I/O objects in this session. Although not strictly necessary
            // for single-threaded contexts, this example code is written to be
            // thread-safe by default.
            boost::beast::net::dispatch(
                std::visit([](auto& s) { return s.get_executor(); }, stream_),
                boost::beast::bind_front_handler(&HttpSession::on_run, shared_from_this(), ctx, router)
            );
        }

        void on_stop()
        {
            // Nothing received yet (idle keep-alive, detect, handshake): cancel, the pending operation
            // completes with operation_aborted and the session ends. Otherwise the request completes,
            // and on_write closes instead of reading the next one.
            // Lingering only keeps the client happy after an error response: a shutdown cuts it short.
            if (lingering_ || (waiting_ && !(parser_ && parser_->got_some())))
                std::visit([](auto& hs) { boost::beast::get_lowest_layer(hs).cancel(); }, stream_);
        }

        void on_run(boost::asio::ssl::context* ctx, std::shared_ptr<gtvr::router::Router> router)
        {
            waiting_ = true;

            std::visit([this, self = shared_from_this(), ctx, router](auto& hs) {
                using T = std::decay_t<decltype(hs)>;
                
                if constexpr (std::is_same_v<T, h_plain_t>) {
                    // Set the timeout.
                    hs.expires_after(options_.http_timeout);

                    boost::beast::async_detect_ssl(hs, buffer_, boost::beast::bind_front_handler(&HttpSession::on_detect, this->shared_from_this(), ctx, std::move(router)));
                }
                else {
                    // SSL only session: nothing to detect, handshake right away
                    boost::beast::get_lowest_layer(hs).expires_after(options_.http_timeout);

                    hs.async_handshake(boost::asio::ssl::stream_base::server, buffer_.data(), boost::beast::bind_front_handler(&HttpSession::on_handshake, this->shared_from_this(), std::move(router)));
                }
            }, stream_);
        }

        void on_detect(boost::asio::ssl::context* ctx, std::shared_ptr<gtvr::router::Router> router, boost::beast::error_code ec, bool result)
        {
            waiting_ = false;

            if (ec)
            {
                // Client closed or timed out before sending anything
                slog_debug("detect: {}", ec.message());
                return;
            }

            if (result)
            {
                // Launch SSL session
                if constexpr (with_ssl == true)
                {
                    on_ssl_session(ctx, router);
                }
                else
                {
                    // TLS client on a plain listener: returning drops the last reference, the socket closes now
                    slog_debug("detect: TLS client on a listener without HTTPS");
                }
            }
            else
            {
                // Launch plain session
                if constexpr (with_plain == true)
                {
                    on_plain_session(router);
                }
            }

        }

        void on_plain_session(std::shared_ptr<gtvr::router::Router> router)
        {
            do_read(router);
        }

        void on_handshake(std::shared_ptr<gtvr::router::Router> router, boost::beast::error_code ec, std::size_t bytes_used)
        {
            waiting_ = false;

            if (ec)
            {
                // Bad certificate, plain HTTP on a TLS port, scanners: the client's problem
                slog_debug("handshake: {}", ec.message());
                return;
            }

            // Consume the portion of the buffer used by the handshake
            buffer_.consume(bytes_used);

            do_read(router);
        }

        void on_shutdown(boost::beast::error_code ec)
        {
            if (ec)
            {
                // Many clients close without a TLS close_notify (stream truncated): harmless
                slog_debug("shutdown: {}", ec.message());
                return;
            }

            // At this point the connection is closed gracefully
        }

        void on_ssl_session(boost::asio::ssl::context* ctx, std::shared_ptr<gtvr::router::Router> router)
        {
            // Try to get a pointer to the existing TCP stream
            if (auto* tcp_ptr = std::get_if<h_plain_t>(&stream_)) {
                
                // Move the underlying socket out of the variant's current state
                // We do this to ensure the socket isn't closed when emplace destroys the old variant index
                auto tcp_moved = std::move(*tcp_ptr);

                // Construct the SSL stream in-place using the moved socket
                stream_.template emplace<h_ssl_t>(std::move(tcp_moved), *ctx);
            }
            
            std::visit([this, router](auto& hs) {
                
                using T = std::decay_t<decltype(hs)>;
                
                if constexpr (std::is_same_v<T, h_ssl_t>) {
                    // Set the timeout.
                    boost::beast::get_lowest_layer(hs).expires_after(options_.http_timeout);
                    
                    // Perform the SSL handshake
                    // Note, this is the buffered version of the handshake.
                    waiting_ = true;
                    hs.async_handshake(boost::asio::ssl::stream_base::server, buffer_.data(), boost::beast::bind_front_handler(&HttpSession::on_handshake, this->shared_from_this(), router));
                }
            }, stream_);
        }

        void do_read(std::shared_ptr<gtvr::router::Router> router)
        {
            waiting_ = true;

            // Construct a new parser for each message
            this->parser_.emplace();

            // Limits to prevent abuse, over them the client gets 413 / 414 / 431
            this->parser_->header_limit(static_cast<std::uint32_t>(options_.http_header_limit));
            this->parser_->body_limit(options_.http_body_limit);
            
            std::visit([this, router, self = shared_from_this()](auto& hs) mutable
            {
                // Set the timeout, for the whole request
                boost::beast::get_lowest_layer(hs).expires_after(options_.http_timeout);
                
                // The header first: a client sending "Expect: 100-continue" waits for our go before the body
                boost::beast::http::async_read_header(hs, buffer_, *parser_, boost::beast::bind_front_handler(&HttpSession::on_read_header, std::move(self), router));
            }, stream_);
        }

        void on_read_header(std::shared_ptr<gtvr::router::Router> router, boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            // Errors (and a body over the limit, known from Content-Length) are answered in on_read
            if (ec)
                return on_read(router, ec, bytes_transferred);

            const auto& request = parser_->get();

            if (request.version() >= 11 && boost::beast::iequals(request[boost::beast::http::field::expect], "100-continue"))
            {
                static constexpr std::string_view continue_response = "HTTP/1.1 100 Continue\r\n\r\n";

                std::visit([this, router, self = shared_from_this()](auto& hs) mutable
                {
                    boost::asio::async_write(hs, boost::asio::buffer(continue_response.data(), continue_response.size()),
                        [this, router, self = std::move(self)](boost::beast::error_code write_ec, std::size_t) mutable
                        {
                            if (write_ec)
                            {
                                slog_debug("write 100-continue: {}", write_ec.message());
                                return;
                            }
                            read_body(std::move(router));
                        });
                }, stream_);
                return;
            }

            read_body(std::move(router));
        }

        void read_body(std::shared_ptr<gtvr::router::Router> router)
        {
            // Continues with the same parser: the header is done, this reads the body (if any)
            std::visit([this, router, self = shared_from_this()](auto& hs) mutable
            {
                boost::beast::http::async_read(hs, buffer_, *parser_, boost::beast::bind_front_handler(&HttpSession::on_read, std::move(self), router));
            }, stream_);
        }

        // Called by the base class
        void do_eof()
        {
            std::visit([this](auto& hs)
            {
                using T = std::decay_t<decltype(hs)>;
                
                if constexpr (std::is_same_v<T, h_ssl_t>)
                {
                    // A TLS shutdown would stop reading what the client still sends: drain instead,
                    // the response is already written
                    if (linger_)
                        return do_linger();

                    // Set the timeout.
                    boost::beast::get_lowest_layer(hs).expires_after(options_.http_timeout);

                    // Perform the SSL shutdown
                    hs.async_shutdown(boost::beast::bind_front_handler(&HttpSession::on_shutdown, this->shared_from_this()));
                }
                else
                {
                    boost::beast::error_code ec;
                    hs.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);

                    if (linger_)
                        do_linger();
                }
                
            }, stream_);
            
            // At this point the connection is closed gracefully
        }

        // Closing a socket with unread data sends a reset, which can make the client lose our error
        // response. So after it, keep reading and discarding until the client closes or the timeout.
        void do_linger()
        {
            lingering_ = true;
            linger_buffer_.resize(16 * 1024);

            // One deadline for the whole drain
            std::visit([this](auto& hs) { boost::beast::get_lowest_layer(hs).expires_after(options_.http_linger_timeout); }, stream_);

            read_linger();
        }

        void read_linger()
        {
            // Over TLS this decrypts and discards, so the client keeps a working connection until we close
            std::visit([this](auto& hs)
            {
                hs.async_read_some(boost::asio::buffer(linger_buffer_), boost::beast::bind_front_handler(&HttpSession::on_linger, shared_from_this()));
            }, stream_);
        }

        void on_linger(boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);

            // EOF, timeout or reset: done, the socket closes with the session
            if (!ec)
                read_linger();
        }

        // The response for a request the parser refused, or nothing when there is no one to answer
        std::optional<boost::beast::http::status> error_status(boost::beast::error_code ec)
        {
            namespace http = boost::beast::http;

            if (ec == http::error::body_limit)
                return http::status::payload_too_large;                     // 413

            if (ec == http::error::header_limit)
            {
                // The parser consumes the request line as soon as it is complete: if there is no target
                // yet, the request line itself (the URL) did not fit, otherwise the headers did not
                return parser_->get().target().empty()
                    ? http::status::uri_too_long                            // 414
                    : http::status::request_header_fields_too_large;        // 431
            }

            // Any other parse error is a malformed request, except a client that left mid-request
            if (ec.category() == http::make_error_code(http::error::bad_method).category()
                && ec != http::error::partial_message
                && ec != http::error::need_more)
                return http::status::bad_request;                           // 400

            return std::nullopt;
        }

        void send_error(boost::beast::http::status status, std::shared_ptr<gtvr::router::Router> router)
        {
            boost::beast::http::response<boost::beast::http::string_body> res{ status, 11 };
            res.set(boost::beast::http::field::content_type, "text/plain");
            res.body() = std::string(boost::beast::http::obsolete_reason(status));
            res.keep_alive(false);
            res.prepare_payload();

            linger_ = true;

            queue_write(boost::beast::http::message_generator(std::move(res)), router);
        }

        
        std::string remote_ip()
        {
            return std::visit([](auto& hs)
            {
                boost::system::error_code ec;

                auto remote_endpoint = boost::beast::get_lowest_layer(hs).socket().remote_endpoint(ec);

                return ec ? std::string("UNKNOWN") : remote_endpoint.address().to_string();
            }, stream_);
        }

        void on_read(std::shared_ptr<gtvr::router::Router> router, boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);

            waiting_ = false;

            // This means they closed the connection
            if (ec == boost::beast::http::error::end_of_stream)
                return do_eof();

            if (ec)
            {
                // A client mistake, not a server failure: debug level, or anyone could flood the logs
                if (auto status = error_status(ec))
                {
                    slog_debug("rejected request ({}): {}", static_cast<int>(*status), ec.message());
                    return send_error(*status, router);
                }

                // Timeout, reset, client gone mid-request: nobody to answer
                slog_debug("read: {}", ec.message());
                return;
            }


            // See if it is a WebSocket Upgrade
            if (boost::beast::websocket::is_upgrade(parser_->get()))
            {
                std::string peer_ip = remote_ip();

                std::visit([this, router, &peer_ip](auto& hs)
                {
                    // Disable the timeout.
                    boost::beast::get_lowest_layer(hs).expires_never();
                    
                    // Create a websocket session, transferring ownership of both the socket and the HTTP request.
                    auto ws = std::make_shared<WebSocketSession<with_plain, with_ssl>>(std::move(hs), parser_->release(), std::move(peer_ip), options_, sessions_);

                    // Routes the upgrade, then accepts or refuses it
                    ws->run(router);
                }, stream_);
            }
            else
            {
                // Send the response
                gtvr::router::HttpRequest req(parser_->get());
                gtvr::router::HttpResponse response;

                // The router works out the client IP from it (trusted proxies)
                req.setPeerIP(remote_ip());
                
                router->route(req, response);

                // Tell the client this connection ends after the response
                if (stopping())
                    response.keepAlive(false);

                queue_write(boost::beast::http::message_generator(std::move(response.beast_res_)), router);
            }
        }
};

}//namespace gtvr
