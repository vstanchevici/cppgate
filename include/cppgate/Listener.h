#pragma once

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <cppgate/Router.h>
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

    /*
        static constexpr bool support_ssl   = has_flag<Flags, ANY_SSL>;
        static constexpr bool support_plain = has_flag<Flags, ANY_PLAIN>;
        static constexpr bool is_websocket  = has_flag<Flags, ANY_WS>;
     */

    // Accepts incoming connections and launches the sessions    
    class Listener : public std::enable_shared_from_this<Listener>
    {
        private:
            boost::beast::net::io_context&          ioc_;
            boost::asio::ssl::context*              ctx_;
            boost::asio::ip::tcp::acceptor          acceptor_;
            std::shared_ptr<gtvr::router::Router>   router_;

        public:
            Listener(boost::beast::net::io_context& ioc, boost::asio::ip::tcp::endpoint endpoint): ioc_(ioc), acceptor_(boost::beast::net::make_strand(ioc))
            {
                boost::beast::error_code ec;

                // Open the acceptor
                acceptor_.open(endpoint.protocol(), ec);
                if(ec)
                {
                    slog_error("open: {}", ec.message());
                    return;
                }

                // Allow address reuse
                acceptor_.set_option(boost::beast::net::socket_base::reuse_address(true), ec);
                if(ec)
                {
                    slog_error("set_option: {}", ec.message());
                    return;
                }

                // Bind to the server address
                acceptor_.bind(endpoint, ec);
                if(ec)
                {
                    slog_error("bind: {}", ec.message());
                    return;
                }

                // Start listening for connections
                acceptor_.listen(boost::beast::net::socket_base::max_listen_connections, ec);
                if(ec)
                {
                    slog_error("listen: {}", ec.message());
                    return;
                }
            }

            // Start accepting incoming connections
            template<uint8_t flags>
            void Run()
            {
                static_assert(flags != 0, "Listener must support at least one protocol!");
                do_accept<flags>();
            }

            void SetSSLContext(boost::asio::ssl::context* ctx)
            {
                ctx_ = ctx;
            }

            void SetRouter(std::shared_ptr<gtvr::router::Router> router)
            {
                router_ = router;
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
                if(ec)
                {
                    slog_error("{}", ec.message());
                    return;
                }
                else
                {
                    static constexpr bool support_ssl   = has_flag<flags, Protocol::ANY_SSL>;
                    static constexpr bool support_plain = has_flag<flags, Protocol::ANY_PLAIN>;
                    
                    // Create the detector http_session and run it
                    if constexpr ((support_ssl == false) && (support_plain == false))
                        std::make_shared<HttpSession<support_plain, support_ssl>>(std::move(socket), ctx_)->run(ctx_, router_);
                    else
                        std::make_shared<HttpSession<support_plain, support_ssl>>(std::move(socket))->run(ctx_, router_);
                }

                // Accept another connection
                do_accept<flags>();
            }
    };


}//namespace gtvr
