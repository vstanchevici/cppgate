#include <catch2/catch_test_macros.hpp>

#include <cppgate/TrustedProxies.h>

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>

namespace http = boost::beast::http;
using gtvr::TrustedProxies;

namespace {

    TrustedProxies proxies(std::initializer_list<const char*> cidrs)
    {
        TrustedProxies p;
        for (auto cidr : cidrs)
            p.add(cidr);
        return p;
    }

    http::fields headers(std::initializer_list<std::pair<const char*, const char*>> list)
    {
        http::fields f;
        for (auto& [name, value] : list)
            f.insert(name, value);
        return f;
    }

    std::string parsed(std::string_view text)
    {
        auto address = TrustedProxies::parseAddress(text);
        return address ? address->to_string() : "<invalid>";
    }

    bool contains(const TrustedProxies& p, const char* address)
    {
        return p.contains(boost::asio::ip::make_address(address));
    }

}

TEST_CASE("parseAddress accepts the forms found in forwarding headers", "[trusted_proxies][parse]")
{
    CHECK(parsed("203.0.113.7") == "203.0.113.7");
    CHECK(parsed("  203.0.113.7\t") == "203.0.113.7");
    CHECK(parsed("203.0.113.7:8080") == "203.0.113.7");
    CHECK(parsed("2001:db8::1") == "2001:db8::1");
    CHECK(parsed("[2001:db8::1]") == "2001:db8::1");
    CHECK(parsed("[2001:db8::1]:4711") == "2001:db8::1");
    CHECK(parsed("\"[2001:db8::1]:4711\"") == "2001:db8::1");
    CHECK(parsed("::ffff:10.0.0.1") == "10.0.0.1");
}

TEST_CASE("parseAddress rejects garbage", "[trusted_proxies][parse]")
{
    CHECK(parsed("") == "<invalid>");
    CHECK(parsed("unknown") == "<invalid>");
    CHECK(parsed("1.2.3") == "<invalid>");
    CHECK(parsed("256.1.1.1") == "<invalid>");
    CHECK(parsed("[2001:db8::1") == "<invalid>");
    CHECK(parsed("UNKNOWN") == "<invalid>");
}

TEST_CASE("add() rejects invalid ranges", "[trusted_proxies][cidr]")
{
    TrustedProxies p;
    CHECK_THROWS_AS(p.add("abc"), std::invalid_argument);
    CHECK_THROWS_AS(p.add("10.0.0.0/33"), std::invalid_argument);
    CHECK_THROWS_AS(p.add("10.0.0.0/x"), std::invalid_argument);
    CHECK_THROWS_AS(p.add("10.0.0.0/"), std::invalid_argument);
    CHECK_THROWS_AS(p.add("fd00::/129"), std::invalid_argument);
    CHECK(p.empty());
}

TEST_CASE("contains() matches ranges and single addresses", "[trusted_proxies][cidr]")
{
    auto p = proxies({ "10.0.0.0/8", "192.168.1.5", "fd00::/8" });

    CHECK(contains(p, "10.0.0.1"));
    CHECK(contains(p, "10.255.255.255"));
    CHECK_FALSE(contains(p, "11.0.0.1"));

    CHECK(contains(p, "192.168.1.5"));
    CHECK_FALSE(contains(p, "192.168.1.6"));

    CHECK(contains(p, "fd12::1"));
    CHECK_FALSE(contains(p, "fe80::1"));

    SECTION("an IPv4-mapped peer from a dual-stack socket matches its IPv4 range")
    {
        CHECK(contains(p, "::ffff:10.1.2.3"));
    }

    SECTION("a range given with host bits set is normalized")
    {
        auto q = proxies({ "10.1.2.3/8" });
        CHECK(contains(q, "10.200.0.1"));
    }
}

TEST_CASE("Without trusted proxies the socket address is the client", "[trusted_proxies][resolve]")
{
    TrustedProxies none;
    auto h = headers({ { "X-Forwarded-For", "1.1.1.1" }, { "X-Real-IP", "2.2.2.2" } });

    CHECK(none.resolve("203.0.113.7", h) == "203.0.113.7");
}

TEST_CASE("Forwarding headers from an untrusted peer are ignored", "[trusted_proxies][resolve][spoofing]")
{
    auto p = proxies({ "10.0.0.0/8" });
    auto h = headers({ { "X-Forwarded-For", "1.1.1.1" }, { "X-Real-IP", "2.2.2.2" } });

    CHECK(p.resolve("203.0.113.7", h) == "203.0.113.7");
}

TEST_CASE("X-Forwarded-For through trusted proxies", "[trusted_proxies][resolve]")
{
    auto p = proxies({ "10.0.0.0/8" });

    SECTION("one proxy")
    {
        CHECK(p.resolve("10.0.0.1", headers({ { "X-Forwarded-For", "203.0.113.7" } })) == "203.0.113.7");
    }

    SECTION("a chain of proxies is skipped right to left")
    {
        CHECK(p.resolve("10.0.0.1", headers({ { "X-Forwarded-For", "203.0.113.7, 10.0.0.3, 10.0.0.2" } })) == "203.0.113.7");
    }

    SECTION("a value the client put on the left is not believed")
    {
        CHECK(p.resolve("10.0.0.1", headers({ { "X-Forwarded-For", "1.1.1.1, 203.0.113.7" } })) == "203.0.113.7");
    }

    SECTION("several X-Forwarded-For headers form one list")
    {
        auto h = headers({ { "X-Forwarded-For", "1.1.1.1" }, { "X-Forwarded-For", "203.0.113.7" } });
        CHECK(p.resolve("10.0.0.1", h) == "203.0.113.7");
    }

    SECTION("only trusted proxies in the chain: the farthest one")
    {
        CHECK(p.resolve("10.0.0.1", headers({ { "X-Forwarded-For", "10.0.0.3, 10.0.0.2" } })) == "10.0.0.3");
    }

    SECTION("garbage in the chain stops at the closest trusted address")
    {
        CHECK(p.resolve("10.0.0.1", headers({ { "X-Forwarded-For", "203.0.113.7, unknown" } })) == "10.0.0.1");
        CHECK(p.resolve("10.0.0.1", headers({ { "X-Forwarded-For", "203.0.113.7, unknown, 10.0.0.2" } })) == "10.0.0.2");
    }

    SECTION("IPv6 client with a port")
    {
        CHECK(p.resolve("10.0.0.1", headers({ { "X-Forwarded-For", "[2001:db8::1]:4711" } })) == "2001:db8::1");
    }

    SECTION("an IPv4-mapped peer is trusted and reported as IPv4")
    {
        CHECK(p.resolve("::ffff:10.0.0.1", headers({})) == "10.0.0.1");
        CHECK(p.resolve("::ffff:10.0.0.1", headers({ { "X-Forwarded-For", "203.0.113.7" } })) == "203.0.113.7");
    }
}

TEST_CASE("X-Real-IP is used when there is no X-Forwarded-For", "[trusted_proxies][resolve]")
{
    auto p = proxies({ "127.0.0.1" });

    CHECK(p.resolve("127.0.0.1", headers({ { "X-Real-IP", "203.0.113.7" } })) == "203.0.113.7");
    CHECK(p.resolve("127.0.0.1", headers({ { "X-Real-IP", "garbage" } })) == "127.0.0.1");
    CHECK(p.resolve("127.0.0.1", headers({ { "X-Forwarded-For", "198.51.100.1" }, { "X-Real-IP", "203.0.113.7" } })) == "198.51.100.1");
}

TEST_CASE("An unparsable peer is returned as is", "[trusted_proxies][resolve]")
{
    auto p = proxies({ "10.0.0.0/8" });
    CHECK(p.resolve("UNKNOWN", headers({ { "X-Forwarded-For", "203.0.113.7" } })) == "UNKNOWN");
}
