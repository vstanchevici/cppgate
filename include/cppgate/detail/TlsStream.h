#pragma once

#include <boost/beast/core/role.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <chrono>
#include <utility>

// Internal: the TLS stream type of the server sessions and the clients
namespace gtvr::detail {

    // After the WebSocket close frames (or at the end of an HTTPS connection), the TLS shutdown waits for
    // the other side's close_notify. Many peers never send it: they wait for us to close TCP instead.
    // The connection is already done at that point, so the TLS goodbye only gets this long.
    inline constexpr std::chrono::milliseconds tls_teardown_grace{ 500 };

    // Our own type, so the teardown below is the one Beast's WebSocket close uses (found by ADL, a better
    // match than Beast's own ssl_stream overload, which waits for close_notify without limit)
    struct TlsStream : boost::beast::ssl_stream<boost::beast::tcp_stream>
    {
        using Base = boost::beast::ssl_stream<boost::beast::tcp_stream>;
        using Base::Base;
    };

    template<class TeardownHandler>
    void async_teardown(boost::beast::role_type, TlsStream& stream, TeardownHandler&& handler)
    {
        boost::beast::get_lowest_layer(stream).expires_after(tls_teardown_grace);
        stream.async_shutdown([&stream, handler = std::forward<TeardownHandler>(handler)](boost::system::error_code) mutable {
            // Answered, truncated or timed out: the close itself is done either way
            boost::system::error_code ignored;
            boost::beast::get_lowest_layer(stream).socket().close(ignored);
            handler(boost::system::error_code{});
        });
    }

    inline void teardown(boost::beast::role_type, TlsStream& stream, boost::system::error_code& ec)
    {
        boost::beast::get_lowest_layer(stream).expires_after(tls_teardown_grace);
        boost::system::error_code ignored;
        stream.shutdown(ignored);
        boost::beast::get_lowest_layer(stream).socket().close(ignored);
        ec = {};
    }

}//namespace gtvr::detail
