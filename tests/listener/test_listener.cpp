#include <catch2/catch_test_macros.hpp>

#include <cppgate/Listener.h>

#include <boost/asio/ssl.hpp>

#include <memory>
#include <stdexcept>

namespace {

    std::shared_ptr<gtvr::Listener> makeListener(boost::asio::io_context& ioc)
    {
        // Port 0: any free port
        auto listener = std::make_shared<gtvr::Listener>(ioc, boost::asio::ip::tcp::endpoint{ boost::asio::ip::make_address("127.0.0.1"), 0 });
        listener->SetRouter(std::make_shared<gtvr::router::Router>());
        return listener;
    }

}

TEST_CASE("HTTPS without an SSL context fails at Run(), not on the first connection", "[listener][ssl]")
{
    boost::asio::io_context ioc;

    CHECK_THROWS_AS(makeListener(ioc)->Run<gtvr::Protocol::HTTPS>(), std::logic_error);
    CHECK_THROWS_AS(makeListener(ioc)->Run<gtvr::Protocol::HTTP | gtvr::Protocol::HTTPS>(), std::logic_error);
    CHECK_THROWS_AS(makeListener(ioc)->Run<gtvr::Protocol::WSS>(), std::logic_error);
}

TEST_CASE("Run() accepts the valid configurations", "[listener][ssl]")
{
    boost::asio::io_context ioc;
    boost::asio::ssl::context ctx{ boost::asio::ssl::context::tls_server };

    CHECK_NOTHROW(makeListener(ioc)->Run<gtvr::Protocol::HTTP | gtvr::Protocol::WS>());

    auto https = makeListener(ioc);
    https->SetSSLContext(&ctx);
    CHECK_NOTHROW(https->Run<gtvr::Protocol::HTTPS | gtvr::Protocol::WSS>());

    auto mixed = makeListener(ioc);
    mixed->SetSSLContext(&ctx);
    CHECK_NOTHROW(mixed->Run<gtvr::Protocol::HTTP | gtvr::Protocol::HTTPS>());
}
