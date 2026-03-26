#pragma once

#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include "WebSocketSession.h"
#include <cppgate/Router.h>
#include <deque>
#include <variant>

namespace gtvr {


// Define stream types
using h_plain_t = boost::beast::tcp_stream;
using h_ssl_t  = boost::beast::ssl_stream<boost::beast::tcp_stream>;

/*
template <typename T>
struct is_variant : std::false_type {};

template <typename... Args>
struct is_variant<std::variant<Args...>> : std::true_type {};

template <typename T>
inline constexpr bool is_variant_v = is_variant<T>::value;

template<typename F>
void access_stream(F&& func) {
    if constexpr (is_variant_v<http_stream_t>) {
        // Multi-protocol: uses std::visit (minimal jump overhead)
        std::visit(std::forward<F>(func), stream_);
    } else {
        // Single protocol: compiler calls the function DIRECTLY
        // Zero runtime cost, identical to calling stream_.read()
        func(stream_);
    }
}
*/

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
class HttpSession : public std::enable_shared_from_this<HttpSession<with_plain, with_ssl>>
{
    static_assert(with_plain || with_ssl, "HttpSession must have or plain or ssl stream.");

    using std::enable_shared_from_this<HttpSession<with_plain, with_ssl>>::shared_from_this;

    private:
        static constexpr std::size_t                                                            queue_limit = 8; // max responses
        std::deque<boost::beast::http::message_generator>                                       response_queue_;
        boost::optional<boost::beast::http::request_parser<boost::beast::http::string_body>>    parser_;
        boost::beast::flat_buffer                                                               buffer_;
        http_stream_t<with_plain, with_ssl>                                                     stream_;
    
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
        bool do_write(std::shared_ptr<gtvr::router::Router> router)
        {
            bool const was_full = response_queue_.size() == queue_limit;

            if (!response_queue_.empty())
            {
                boost::beast::http::message_generator msg = std::move(response_queue_.front());

                bool keep_alive = msg.keep_alive();

                std::visit([this, &msg, router, keep_alive](auto& hs)
                {
                    boost::beast::async_write(hs, std::move(msg), boost::beast::bind_front_handler(&HttpSession::on_write, shared_from_this(), router, keep_alive));
                }, stream_);
            }

            return was_full;
        }

        void on_write(std::shared_ptr<gtvr::router::Router> router, bool keep_alive, boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);

            if (ec)
            {
                slog_error("{}", ec.message());
                return;
            }

            if (!keep_alive)
            {
                // This means we should close the connection, usually because
                // the response indicated the "Connection: close" semantic.
                return do_eof();
            }

            response_queue_.pop_front();
            
            // Inform the queue that a write completed
            if (do_write(router))
            {
                // Read another request
                do_read(router);
            }
        }


    public:
        explicit HttpSession(boost::asio::ip::tcp::socket&& socket): stream_(boost::beast::tcp_stream(std::move(socket)))
        {
        }
        
        explicit HttpSession(boost::asio::ip::tcp::socket&& socket, boost::asio::ssl::context* ctx): stream_(std::in_place_type<h_ssl_t>, std::move(socket), *ctx)
        {
            
        }
        
    
        // Launch the detector
        void run(boost::asio::ssl::context* ctx, std::shared_ptr<gtvr::router::Router> router)
        {
            // We need to be executing within a strand to perform async operations
            // on the I/O objects in this session. Although not strictly necessary
            // for single-threaded contexts, this example code is written to be
            // thread-safe by default.
            boost::beast::net::dispatch(
                std::visit([](auto& s) { return s.get_executor(); }, stream_),
                boost::beast::bind_front_handler(&HttpSession::on_run, shared_from_this(), ctx, router)
            );
        }

        void on_run(boost::asio::ssl::context* ctx, std::shared_ptr<gtvr::router::Router> router)
        {
            std::visit([this, self = shared_from_this(), ctx, router](auto& hs) {
                using T = std::decay_t<decltype(hs)>;
                
                if constexpr (std::is_same_v<T, h_plain_t>) {
                    // Set the timeout.
                    hs.expires_after(std::chrono::seconds(30));

                    boost::beast::async_detect_ssl(hs, buffer_, boost::beast::bind_front_handler(&HttpSession::on_detect, shared_from_this(), ctx, std::move(router)));
                }
            }, stream_);
        }

        void on_detect(boost::asio::ssl::context* ctx, std::shared_ptr<gtvr::router::Router> router, boost::beast::error_code ec, bool result)
        {
            if (ec)
            {
                slog_error("{}", ec.message());
                return;
            }

            if (result)
            {
                // Launch SSL session
                if constexpr (with_ssl == true)
                {
                    on_ssl_session(ctx, router);
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
            if (ec)
            {
                slog_error("{}", ec.message());
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
                slog_error("{}", ec.message());
                return;
            }

            // At this point the connection is closed gracefully
        }

        void on_ssl_session(boost::asio::ssl::context* ctx, std::shared_ptr<gtvr::router::Router> router)
        {
            /*
             stream_ = std::visit([&ctx](auto& hs) -> http_stream_t<with_plain, with_ssl> {
                                 using T = std::decay_t<decltype(hs)>;
                                 if constexpr (std::is_same_v<T, plain_t>) {
                                     return ssl_t(std::move(hs), ctx);
                                 } else {
                                     return std::move(hs);
                                 }
                             }, std::move(stream_));
             */
            
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
                    boost::beast::get_lowest_layer(hs).expires_after(std::chrono::seconds(30));
                    
                    // Perform the SSL handshake
                    // Note, this is the buffered version of the handshake.
                    hs.async_handshake(boost::asio::ssl::stream_base::server, buffer_.data(), boost::beast::bind_front_handler(&HttpSession::on_handshake, shared_from_this(), router));
                }
            }, stream_);
        }

        void do_read(std::shared_ptr<gtvr::router::Router> router)
        {
            // Construct a new parser for each message
            this->parser_.emplace();

            // Apply a reasonable limit to the allowed size
            // of the body in bytes to prevent abuse.
            this->parser_->body_limit(10000);
            
            std::visit([this, router, self = shared_from_this()](auto& hs) mutable
            {
                // Set the timeout.
                boost::beast::get_lowest_layer(hs).expires_after(std::chrono::seconds(30));
                
                // Read a request using the parser-oriented interface
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
                    // Set the timeout.
                    boost::beast::get_lowest_layer(hs).expires_after(std::chrono::seconds(30));

                    // Perform the SSL shutdown
                    hs.async_shutdown(boost::beast::bind_front_handler(&HttpSession::on_shutdown, shared_from_this()));
                }
                else
                {
                    boost::beast::error_code ec;
                    hs.socket().shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
                }
                
            }, stream_);
            
            // At this point the connection is closed gracefully
        }

        
        void on_read(std::shared_ptr<gtvr::router::Router> router, boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);

            // This means they closed the connection
            if (ec == boost::beast::http::error::end_of_stream)
                return do_eof();

            if (ec)
            {
                slog_error("{}", ec.message());
                return;
            }


            // See if it is a WebSocket Upgrade
            if (boost::beast::websocket::is_upgrade(parser_->get()))
            {
                std::visit([this, router](auto& hs)
                {
                    // Disable the timeout.
                    boost::beast::get_lowest_layer(hs).expires_never();
                    
                    // Create a websocket session, transferring ownership of both the socket and the HTTP request.
                    auto ws = std::make_shared<WebSocketSession<with_plain, with_ssl>>(std::move(hs));
                    ws->run(std::move(parser_->release()), router);
                }, stream_);
            }
            else
            {
                // Send the response
                bool req_keep_alive = parser_->keep_alive();

                gtvr::router::HttpResponse res;
                gtvr::router::HttpRequest req(parser_->get(), router->getRouterPamasObject());
                router->route(req, res);

                queue_write(boost::beast::http::message_generator(std::move(res)), router);

                // If we aren't at the queue limit, try to pipeline another request
                if (response_queue_.size() < queue_limit)
                {
                    if (req_keep_alive)
                        do_read(router);
                    else
                        do_eof();
                }
            }
        }
};

}//namespace gtvr
