#pragma once

// Internal, shared by HttpClient and WebSocketClient

#include <cppgate/Client/ClientTlsOptions.h>
#include <boost/asio/ssl/context.hpp>
#include <boost/system/result.hpp>
#include <string>
#include <string_view>

namespace gtvr::detail {

    struct Target
    {
        bool        tls = false;
        std::string host;           // for DNS and SNI, IPv6 without brackets
        std::string port;
        std::string host_header;    // "example.com" or "example.com:8080"
        std::string target;         // "/path?query"
        std::string key;            // connection reuse: scheme, host, port
    };

    enum class UrlKind { http, websocket };

    // http/https, or ws/wss; errors are client_error::invalid_url / unsupported_scheme
    boost::system::result<Target> parseUrl(std::string_view url, UrlKind kind);

    // TLS 1.2+, verify mode and the trusted CAs from the options. Throws boost::system::system_error
    // for a CA that can't be loaded, std::invalid_argument when verify_peer has no CA source.
    // `who` prefixes the messages ("HttpClient", "WebSocketClient").
    void configureClientTls(boost::asio::ssl::context& tls, const ClientTlsOptions& options, const char* who);

}//namespace gtvr::detail
