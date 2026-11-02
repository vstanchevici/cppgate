#pragma once

#include <boost/asio/dispatch.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/system_error.hpp>
#include <cppgate/Router.h>
#include <cppgate/ServerOptions.h>
#include <cppgate/SessionRegistry.h>
#include <chrono>
#include <stdexcept>
#include "HttpSession.h"

namespace gtvr {  

    // By default, enums are not bitmasks
    template<typename E>
    struct is_bitmask_enum : std::false_type {};

    // Use a template to enable operators only for marked enums
    template<typename E>
    requires is_bitmask_enum<E>::value
    constexpr E operator|(E lhs, E rhs) { // Enable the bitwise OR operator
        using T = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<T>(lhs) | static_cast<T>(rhs));
    }

    template<typename E>
    requires is_bitmask_enum<E>::value
    constexpr E operator&(E lhs, E rhs) {     // Enable the bitwise AND operator
        using T = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<T>(lhs) & static_cast<T>(rhs));
    }

    enum Protocol : uint8_t {
        NONE  = 0,
        HTTP  = 1 << 0,
        HTTPS = 1 << 1,
        WS    = 1 << 2,
        WSS   = 1 << 3,
        // Helpers
        ANY_SSL   = HTTPS | WSS,
        ANY_PLAIN = HTTP | WS,
        ANY_WS    = WS | WSS
    };

    template<>
    struct is_bitmask_enum<Protocol> : std::true_type {};

    
    template<uint8_t flags, uint8_t mask>
    static constexpr bool has_flag = (flags & mask) != 0;

    // Accepts incoming connections and launches the sessions    
    class Listener : public std::enable_shared_from_this<Listener>
    {
        private:
            boost::beast::net::io_context&          ioc_;
            boost::asio::ssl::context*              ctx_ = nullptr;
            boost::asio::ip::tcp::acceptor          acceptor_;
            boost::asio::steady_timer               accept_retry_timer_;
            std::shared_ptr<gtvr::router::Router>   router_;
            ServerOptions                           options_;
            std::shared_ptr<detail::SessionRegistry> sessions_ = std::make_shared<detail::SessionRegistry>();

        public:
            // Throws boost::system::system_error when the address can't be used (e.g. port already in use)
            Listener(boost::beast::net::io_context& ioc, boost::asio::ip::tcp::endpoint endpoint): ioc_(ioc), acceptor_(boost::beast::net::make_strand(ioc)), accept_retry_timer_(acceptor_.get_executor())
            {
                boost::beast::error_code ec;

                // Open the acceptor
                acceptor_.open(endpoint.protocol(), ec);
                if(ec)
                    throw boost::system::system_error(ec, "Listener: open");

                // Allow address reuse
                acceptor_.set_option(boost::beast::net::socket_base::reuse_address(true), ec);
                if(ec)
                    throw boost::system::system_error(ec, "Listener: set_option");

                // Bind to the server address
                acceptor_.bind(endpoint, ec);
                if(ec)
                    throw boost::system::system_error(ec, "Listener: bind " + endpoint.address().to_string() + ":" + std::to_string(endpoint.port()));

                // Start listening for connections
                acceptor_.listen(boost::beast::net::socket_base::max_listen_connections, ec);
                if(ec)
                    throw boost::system::system_error(ec, "Listener: listen");
            }

            // Start accepting incoming connections.
            // HTTP | HTTPS on the same port: each connection is checked for TLS. HTTPS alone: TLS only.
            // WebSocket upgrades are accepted on any of them (WS over HTTP, WSS over HTTPS).
            template<uint8_t flags>
            void run()
            {
                static_assert(flags != 0, "Listener must support at least one protocol!");

                if constexpr (has_flag<flags, Protocol::ANY_SSL>)
                {
                    if (ctx_ == nullptr)
                        throw std::logic_error("Listener: HTTPS/WSS needs setSSLContext() before run()");
                }

                do_accept<flags>();
            }

            // Graceful shutdown, from any thread (e.g. a boost::asio::signal_set handler):
            // stops accepting, closes idle connections, lets requests in progress finish (with
            // Connection: close) and closes WebSockets with 1001. io_context::run() returns once
            // every connection is gone, unless something else keeps it busy.
            void stop()
            {
                boost::beast::net::post(acceptor_.get_executor(), [self = shared_from_this()]() {
                    boost::beast::error_code ec;
                    self->accept_retry_timer_.cancel();
                    self->acceptor_.close(ec);
                });

                sessions_->stopAll();
            }

            // The bound address, useful with port 0 (any free port)
            boost::asio::ip::tcp::endpoint localEndpoint() const
            {
                boost::beast::error_code ec;
                return acceptor_.local_endpoint(ec);
            }

            // Live connections (HTTP and WebSocket)
            std::size_t connectionCount() const
            {
                return sessions_->size();
            }

            // The context must outlive the Listener, only the pointer is kept
            void setSSLContext(boost::asio::ssl::context* ctx)
            {
                ctx_ = ctx;
            }

            void setRouter(std::shared_ptr<gtvr::router::Router> router)
            {
                router_ = router;
            }

            // Limits and timeouts, call before run()
            void setOptions(const ServerOptions& options)
            {
                options_ = options;
            }
        
        private:
            template<uint8_t flags>
            void do_accept()
            {
                // The new connection gets its own strand
                acceptor_.async_accept(boost::beast::net::make_strand(ioc_), boost::beast::bind_front_handler(&Listener::on_accept<flags>, shared_from_this()));
            }

            template<uint8_t flags>
            void on_accept(boost::beast::error_code ec, boost::asio::ip::tcp::socket socket)
            {
                // stop() closed the acceptor
                if (ec == boost::asio::error::operation_aborted || !acceptor_.is_open())
                    return;

                if(ec)
                {
                    // The client gave up before we accepted it: nothing wrong on our side
                    if (ec == boost::asio::error::connection_aborted || ec == boost::asio::error::connection_reset)
                    {
                        slog_debug("accept: {}", ec.message());
                        return do_accept<flags>();
                    }

                    // Out of file descriptors or memory: keep the listener alive, but give the
                    // server a moment to release resources instead of failing in a busy loop
                    slog_error("accept: {} (retrying in 100 ms)", ec.message());

                    accept_retry_timer_.expires_after(std::chrono::milliseconds(100));
                    accept_retry_timer_.async_wait([self = shared_from_this()](boost::beast::error_code wait_ec) {
                        if (!wait_ec && self->acceptor_.is_open())
                            self->do_accept<flags>();
                    });
                    return;
                }

                static constexpr bool support_ssl   = has_flag<flags, Protocol::ANY_SSL>;
                static constexpr bool support_plain = has_flag<flags, Protocol::ANY_PLAIN>;
                
                // With plain support the session starts plain and detects TLS (when supported),
                // SSL only: TLS from the first byte
                if constexpr (support_plain)
                    std::make_shared<HttpSession<support_plain, support_ssl>>(std::move(socket), options_, sessions_)->run(ctx_, router_);
                else
                    std::make_shared<HttpSession<support_plain, support_ssl>>(std::move(socket), ctx_, options_, sessions_)->run(ctx_, router_);

                // Accept another connection
                do_accept<flags>();
            }
    };


}//namespace gtvr
