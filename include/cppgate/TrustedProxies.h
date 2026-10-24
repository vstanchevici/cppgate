#pragma once

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <boost/beast/http/fields.hpp>
#include <cppgate/cppgate.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gtvr {

    /*
        Finds the real client IP of a request that may have come through reverse proxies.

        Forwarding headers (X-Forwarded-For, X-Real-IP) are only read when the socket peer is a
        trusted proxy, any client can send them. With no trusted proxies (the default) the client
        IP is always the socket peer.

        X-Forwarded-For is read right to left, skipping trusted proxies: the first untrusted
        address is the client. The leftmost entries are whatever the client sent, never trust them.
    */
    class CPPGATE_API TrustedProxies
    {
        private:
            std::vector<boost::asio::ip::network_v4> v4_;
            std::vector<boost::asio::ip::network_v6> v6_;

        public:
            // "10.0.0.0/8", "fd00::/8", or a single address "127.0.0.1". Throws std::invalid_argument.
            void add(std::string_view cidr);

            bool empty() const noexcept { return v4_.empty() && v6_.empty(); }

            bool contains(const boost::asio::ip::address& address) const;

            // Client IP of a request received from `peer` (the socket address)
            std::string resolve(std::string_view peer, const boost::beast::http::fields& headers) const;

            // Accepts "1.2.3.4", "1.2.3.4:80", "2001:db8::1", "[2001:db8::1]:80", optionally quoted.
            // IPv4-mapped IPv6 (::ffff:1.2.3.4) is returned as IPv4.
            static std::optional<boost::asio::ip::address> parseAddress(std::string_view text);
    };

}//namespace gtvr
