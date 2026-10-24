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
        listener->setRouter(std::make_shared<gtvr::router::Router>());
        return listener;
    }

}

TEST_CASE("HTTPS without an SSL context fails at run(), not on the first connection", "[listener][ssl]")
{
    boost::asio::io_context ioc;

    CHECK_THROWS_AS(makeListener(ioc)->run<gtvr::Protocol::HTTPS>(), std::logic_error);
    CHECK_THROWS_AS(makeListener(ioc)->run<gtvr::Protocol::HTTP | gtvr::Protocol::HTTPS>(), std::logic_error);
    CHECK_THROWS_AS(makeListener(ioc)->run<gtvr::Protocol::WSS>(), std::logic_error);
}

TEST_CASE("run() accepts the valid configurations", "[listener][ssl]")
{
    boost::asio::io_context ioc;
    boost::asio::ssl::context ctx{ boost::asio::ssl::context::tls_server };

    CHECK_NOTHROW(makeListener(ioc)->run<gtvr::Protocol::HTTP | gtvr::Protocol::WS>());

    auto https = makeListener(ioc);
    https->setSSLContext(&ctx);
    CHECK_NOTHROW(https->run<gtvr::Protocol::HTTPS | gtvr::Protocol::WSS>());

    auto mixed = makeListener(ioc);
    mixed->setSSLContext(&ctx);
    CHECK_NOTHROW(mixed->run<gtvr::Protocol::HTTP | gtvr::Protocol::HTTPS>());
}

#include <boost/beast/websocket.hpp>

#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#ifndef _WIN32
    #include <sys/socket.h>
    #include <sys/time.h>
#endif

namespace {

    namespace http = boost::beast::http;
    using tcp = boost::asio::ip::tcp;

    void initLoggerOnce()
    {
        // Sessions log at debug level, the logger must exist
        static std::once_flag once;
        std::call_once(once, [] { gtvr::Logger::Init(); });
    }

    // Blocking reads give up after this, so a server that never closes fails the test instead of hanging it
    void setReceiveTimeout(tcp::socket& socket, int ms)
    {
        #ifdef _WIN32
            DWORD timeout = ms;
            setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        #else
            timeval tv{ ms / 1000, (ms % 1000) * 1000 };
            setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        #endif
    }

    struct ReadResult
    {
        std::string data;
        bool closed_by_server = false;     // EOF, as opposed to the receive timeout
    };

    // Reads until `until` is in the data, or until the server closes / the timeout
    ReadResult readFrom(tcp::socket& socket, const std::string& until = {})
    {
        ReadResult result;
        char buffer[4096];

        for (;;)
        {
            boost::system::error_code ec;
            auto n = socket.read_some(boost::asio::buffer(buffer), ec);
            result.data.append(buffer, n);

            if (ec)
            {
                result.closed_by_server = (ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset);
                return result;
            }

            if (!until.empty() && result.data.find(until) != std::string::npos)
                return result;
        }
    }

    struct Server
    {
        boost::asio::io_context ioc;
        std::shared_ptr<gtvr::router::Router> router = std::make_shared<gtvr::router::Router>();
        std::shared_ptr<gtvr::Listener> listener;
        gtvr::ServerOptions options;
        std::future<void> running;

        Server()
        {
            initLoggerOnce();
        }

        void start()
        {
            listener = std::make_shared<gtvr::Listener>(ioc, tcp::endpoint{ boost::asio::ip::make_address("127.0.0.1"), 0 });
            listener->setRouter(router);
            listener->setOptions(options);
            listener->run<gtvr::Protocol::HTTP | gtvr::Protocol::WS>();
            running = std::async(std::launch::async, [this] { ioc.run(); });
        }

        tcp::endpoint endpoint() const
        {
            return listener->localEndpoint();
        }

        // True when io_context::run() returned by itself within the timeout
        bool stoppedWithin(std::chrono::milliseconds timeout)
        {
            if (running.wait_for(timeout) == std::future_status::ready)
                return true;

            ioc.stop();
            running.wait();
            return false;
        }
    };

}

TEST_CASE("The constructor throws when the port is already in use", "[listener]")
{
    boost::asio::io_context ioc;
    auto first = makeListener(ioc);

    CHECK_THROWS_AS(std::make_shared<gtvr::Listener>(ioc, first->localEndpoint()), boost::system::system_error);
}

TEST_CASE("stop() closes an idle keep-alive connection and run() returns", "[listener][stop]")
{
    Server server;
    server.router->add({ http::verb::get }, "/hello", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
        res.sendContent("hello");
    });
    server.start();

    boost::asio::io_context client_ioc;
    tcp::socket client(client_ioc);
    client.connect(server.endpoint());
    setReceiveTimeout(client, 3000);

    boost::asio::write(client, boost::asio::buffer(std::string("GET /hello HTTP/1.1\r\nHost: x\r\n\r\n")));
    auto first = readFrom(client, "hello");
    REQUIRE(first.data.find("200 OK") != std::string::npos);

    // The connection now waits for its next request: the 30 s read timeout must not be needed
    auto t0 = std::chrono::steady_clock::now();
    server.listener->stop();

    CHECK(readFrom(client).closed_by_server);
    CHECK(server.stoppedWithin(std::chrono::seconds(2)));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
    CHECK(server.listener->connectionCount() == 0);
}

TEST_CASE("stop() lets a request in progress finish with Connection: close", "[listener][stop]")
{
    Server server;
    std::promise<void> handler_started;

    server.router->add({ http::verb::get }, "/slow", [&](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
        handler_started.set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        res.sendContent("slow done");
    });
    server.start();

    boost::asio::io_context client_ioc;
    tcp::socket client(client_ioc);
    client.connect(server.endpoint());
    setReceiveTimeout(client, 3000);
    boost::asio::write(client, boost::asio::buffer(std::string("GET /slow HTTP/1.1\r\nHost: x\r\n\r\n")));

    // Stop while the handler runs
    handler_started.get_future().wait();
    server.listener->stop();

    auto response = readFrom(client);
    CHECK(response.data.find("200 OK") != std::string::npos);
    CHECK(response.data.find("slow done") != std::string::npos);
    CHECK(response.data.find("Connection: close") != std::string::npos);
    CHECK(response.closed_by_server);
    CHECK(server.stoppedWithin(std::chrono::seconds(2)));
}

TEST_CASE("stop() closes WebSockets with 1001 going away", "[listener][stop][websocket]")
{
    Server server;
    std::promise<void> opened;
    std::promise<gtvr::WebSocketContext::CloseCode> server_close_code;

    server.router->add("/ws", [&](std::shared_ptr<gtvr::WebSocketContext> ws) {
        ws->onClose([&](gtvr::WebSocketContext&, gtvr::WebSocketContext::CloseCode code) { server_close_code.set_value(code); });
        opened.set_value();
        return true;
    });
    server.start();

    boost::asio::io_context client_ioc;
    boost::beast::websocket::stream<tcp::socket> client(client_ioc);
    client.next_layer().connect(server.endpoint());
    setReceiveTimeout(client.next_layer(), 3000);
    client.handshake("127.0.0.1", "/ws");

    opened.get_future().wait();
    server.listener->stop();

    boost::beast::flat_buffer buffer;
    boost::system::error_code ec;
    client.read(buffer, ec);

    CHECK(ec == boost::beast::websocket::error::closed);
    CHECK(client.reason().code == boost::beast::websocket::close_code::going_away);

    auto code = server_close_code.get_future();
    REQUIRE(code.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(code.get() == gtvr::WebSocketContext::CloseCode::going_away);

    CHECK(server.stoppedWithin(std::chrono::seconds(2)));
}

TEST_CASE("After stop() new connections are refused", "[listener][stop]")
{
    Server server;
    server.start();
    auto endpoint = server.endpoint();

    server.listener->stop();
    REQUIRE(server.stoppedWithin(std::chrono::seconds(2)));

    boost::asio::io_context client_ioc;
    tcp::socket client(client_ioc);
    boost::system::error_code ec;
    client.connect(endpoint, ec);
    CHECK(ec == boost::asio::error::connection_refused);
}

TEST_CASE("Expect: 100-continue gets a 100 Continue before the body", "[listener][http]")
{
    Server server;
    server.options.http_body_limit = 1024;
    server.router->add({ http::verb::post }, "/upload", [](gtvr::router::HttpRequest& req, gtvr::router::HttpResponse& res) {
        res.sendContent("got " + std::to_string(req.body().size()));
    });
    server.start();

    boost::asio::io_context client_ioc;
    tcp::socket client(client_ioc);
    client.connect(server.endpoint());

    SECTION("accepted: 100 Continue, then the body, then the response")
    {
        // Well under the 1 s a client would wait before sending the body anyway
        setReceiveTimeout(client, 500);
        boost::asio::write(client, boost::asio::buffer(std::string("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n")));

        auto interim = readFrom(client, "\r\n\r\n");
        CHECK(interim.data == "HTTP/1.1 100 Continue\r\n\r\n");

        boost::asio::write(client, boost::asio::buffer(std::string("hello")));
        auto response = readFrom(client, "got 5");
        CHECK(response.data.find("200 OK") != std::string::npos);
        CHECK(response.data.find("got 5") != std::string::npos);
    }

    SECTION("too large: 413 right away, the body is never requested")
    {
        setReceiveTimeout(client, 2000);
        boost::asio::write(client, boost::asio::buffer(std::string("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\nExpect: 100-continue\r\n\r\n")));

        auto response = readFrom(client);
        CHECK(response.data.find("413") != std::string::npos);
        CHECK(response.data.find("100 Continue") == std::string::npos);
    }

    SECTION("without Expect nothing changes")
    {
        setReceiveTimeout(client, 2000);
        boost::asio::write(client, boost::asio::buffer(std::string("POST /upload HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello")));

        auto response = readFrom(client, "got 5");
        CHECK(response.data.rfind("HTTP/1.1 200 OK", 0) == 0);
    }

    server.listener->stop();
    CHECK(server.stoppedWithin(std::chrono::seconds(2)));
}

TEST_CASE("A WebSocket client that doesn't read is disconnected with 1008", "[listener][websocket]")
{
    Server server;
    server.options.websocket_send_queue_limit = 4;

    constexpr int sent = 200;
    std::promise<std::size_t> pending_after_send;
    std::promise<gtvr::WebSocketContext::CloseCode> server_close_code;

    server.router->add("/flood", [&](std::shared_ptr<gtvr::WebSocketContext> ws) {
        ws->onClose([&](gtvr::WebSocketContext&, gtvr::WebSocketContext::CloseCode code) { server_close_code.set_value(code); });

        // 200 x 256 KB: far more than the socket buffers, the queue must overflow
        auto payload = std::make_shared<const std::string>(256 * 1024, 'x');
        for (int i = 0; i < sent; ++i)
            ws->sendText(payload);

        pending_after_send.set_value(ws->pendingMessages());
        return true;
    });
    server.start();

    boost::asio::io_context client_ioc;
    boost::beast::websocket::stream<tcp::socket> client(client_ioc);
    client.next_layer().connect(server.endpoint());
    setReceiveTimeout(client.next_layer(), 5000);
    client.read_message_max(1024 * 1024);
    client.handshake("127.0.0.1", "/flood");

    // Nothing was written yet when the handler returned: every message is pending
    auto pending = pending_after_send.get_future();
    REQUIRE(pending.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(pending.get() == sent);

    // Now read what made it, until the close frame
    int received = 0;
    boost::system::error_code ec;
    for (;;)
    {
        boost::beast::flat_buffer buffer;
        client.read(buffer, ec);
        if (ec)
            break;
        ++received;
    }

    CHECK(ec == boost::beast::websocket::error::closed);
    CHECK(client.reason().code == boost::beast::websocket::close_code::policy_error);
    CHECK(received < sent);

    auto code = server_close_code.get_future();
    REQUIRE(code.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(code.get() == gtvr::WebSocketContext::CloseCode::policy_error);

    server.listener->stop();
    CHECK(server.stoppedWithin(std::chrono::seconds(2)));
}
