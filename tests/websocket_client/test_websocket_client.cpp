#include <catch2/catch_test_macros.hpp>

#include <cppgate/Client/WebSocketClient.h>
#include <cppgate/Listener.h>

#include <boost/asio/ssl.hpp>

#include "../http_client/TestCertificates.h"

#ifndef _WIN32
    #include <sys/socket.h>
    #include <sys/time.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;
using gtvr::WebSocketClient;
using gtvr::client_error;
using CloseCode = WebSocketClient::CloseCode;

namespace {

    void initLoggerOnce()
    {
        static std::once_flag once;
        std::call_once(once, [] { gtvr::Logger::Init(); });
    }

    // What the server saw
    struct ServerLog
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<CloseCode> closes;

        void closed(CloseCode code)
        {
            std::lock_guard lock(mutex);
            closes.push_back(code);
            cv.notify_all();
        }

        bool waitCloses(std::size_t n, std::chrono::milliseconds timeout = 3s)
        {
            std::unique_lock lock(mutex);
            return cv.wait_for(lock, timeout, [&] { return closes.size() >= n; });
        }
    };

    // An in-process cppgate server with WebSocket routes, plain or HTTP+HTTPS on one port
    struct Server
    {
        boost::asio::io_context ioc;
        boost::asio::ssl::context tls{ boost::asio::ssl::context::tls_server };
        std::shared_ptr<gtvr::router::Router> router = std::make_shared<gtvr::router::Router>();
        std::shared_ptr<gtvr::Listener> listener;
        std::future<void> running;
        unsigned short port = 0;
        std::shared_ptr<ServerLog> log = std::make_shared<ServerLog>();

        explicit Server(bool with_tls = false, unsigned short fixed_port = 0)
        {
            initLoggerOnce();
            auto server_log = log;

            auto track = [server_log](gtvr::WebSocketContext& ws) {
                ws.onClose([server_log](gtvr::WebSocketContext&, CloseCode code) { server_log->closed(code); });
            };

            router->add("/echo", [track](std::shared_ptr<gtvr::WebSocketContext> ws) {
                track(*ws);
                ws->onMessage([](gtvr::WebSocketContext& ws, const gtvr::WebSocketMessage& msg) {
                    if (msg.isText())
                        ws.sendText(std::string(msg.text()));
                    else
                        ws.sendBinary(std::vector<std::uint8_t>(msg.bytes().begin(), msg.bytes().end()));
                });
                return true;
            });

            // Sends back what the client asked for in the upgrade
            router->add("/headers", [track](std::shared_ptr<gtvr::WebSocketContext> ws) {
                track(*ws);
                ws->sendText(std::string(ws->request()[http::field::sec_websocket_protocol]) + "|" + std::string(ws->request()["X-Test"]));
                return true;
            });

            gtvr::router::Handler auth = gtvr::router::WebSocketHandler([](std::shared_ptr<gtvr::WebSocketContext> ws) {
                if (ws->request()[http::field::authorization] == "Bearer ok")
                    return true;
                ws->reject(http::status::unauthorized, "who are you?");
                return false;
            });
            router->add("/auth", { auth }, [track](std::shared_ptr<gtvr::WebSocketContext> ws) { track(*ws); return true; });

            // Starts sending as soon as the connection opens, like servers that greet
            router->add("/greet", [track](std::shared_ptr<gtvr::WebSocketContext> ws) {
                track(*ws);
                for (int i = 0; i < 5; ++i)
                    ws->sendText("welcome " + std::to_string(i));
                return true;
            });

            // Closes with 1008 when told "bye"
            router->add("/policy", [track](std::shared_ptr<gtvr::WebSocketContext> ws) {
                track(*ws);
                ws->onMessage([](gtvr::WebSocketContext& ws, const gtvr::WebSocketMessage& msg) {
                    if (msg.text() == "bye")
                        ws.close(CloseCode::policy_error);
                });
                return true;
            });

            listener = std::make_shared<gtvr::Listener>(ioc, tcp::endpoint{ boost::asio::ip::make_address("127.0.0.1"), fixed_port });
            listener->setRouter(router);

            if (with_tls)
            {
                tls.use_certificate_chain(boost::asio::buffer(std::string_view(test_certificates::server_cert)));
                tls.use_private_key(boost::asio::buffer(std::string_view(test_certificates::server_key)), boost::asio::ssl::context::pem);
                listener->setSSLContext(&tls);
                listener->run<gtvr::Protocol::HTTP | gtvr::Protocol::HTTPS>();
            }
            else
            {
                listener->run<gtvr::Protocol::HTTP>();
            }

            port = listener->localEndpoint().port();
            running = std::async(std::launch::async, [this] { ioc.run(); });
        }

        std::string url(const std::string& path, const char* scheme = "ws", const char* host = "127.0.0.1") const
        {
            return std::string(scheme) + "://" + host + ":" + std::to_string(port) + path;
        }

        void stop()
        {
            if (!listener)
                return;
            listener->stop();
            if (running.wait_for(5s) != std::future_status::ready)
                ioc.stop();
            running.wait();
            listener.reset();
        }

        ~Server()
        {
            stop();
        }
    };

    // Everything the client reported, in order
    struct Recorder
    {
        std::mutex mutex;
        std::condition_variable cv;
        int opens = 0;
        std::vector<CloseCode> closes;
        std::vector<boost::system::error_code> errors;
        std::vector<std::string> messages;
        std::vector<bool> text;
        std::vector<unsigned> attempts;
        std::vector<std::chrono::milliseconds> delays;
        std::vector<std::thread::id> threads;

        WebSocketClient::Callbacks callbacks()
        {
            WebSocketClient::Callbacks cb;
            cb.onOpen = [this](WebSocketClient::Connection&) { update([&] { ++opens; }); };
            cb.onClose = [this](WebSocketClient::Connection&, CloseCode code) { update([&] { closes.push_back(code); }); };
            cb.onError = [this](WebSocketClient::Connection&, boost::system::error_code ec) { update([&] { errors.push_back(ec); }); };
            cb.onMessage = [this](WebSocketClient::Connection&, const gtvr::WebSocketMessage& msg) {
                update([&] {
                    messages.emplace_back(msg.text());
                    text.push_back(msg.isText());
                    threads.push_back(std::this_thread::get_id());
                });
            };
            cb.onReconnecting = [this](WebSocketClient::Connection&, unsigned attempt, std::chrono::milliseconds delay) {
                update([&] { attempts.push_back(attempt); delays.push_back(delay); });
            };
            return cb;
        }

        template<typename F>
        void update(F&& f)
        {
            std::lock_guard lock(mutex);
            f();
            cv.notify_all();
        }

        template<typename P>
        bool waitFor(P predicate, std::chrono::milliseconds timeout = 3s)
        {
            std::unique_lock lock(mutex);
            return cv.wait_for(lock, timeout, [&] { return predicate(*this); });
        }
    };

    WebSocketClient::Options trustingTestCa()
    {
        WebSocketClient::Options options;
        options.use_bundled_cas = false;
        options.extra_ca_pem = { test_certificates::ca };
        return options;
    }

}

TEST_CASE("Connect, echo text and binary, close", "[wsclient]")
{
    Server server;
    WebSocketClient client;
    Recorder rec;

    auto ws = client.connect({ .url = server.url("/echo") }, rec.callbacks());
    REQUIRE(rec.waitFor([](auto& r) { return r.opens == 1; }));
    CHECK(ws->isConnected());

    CHECK(ws->sendText("hello"));
    CHECK(ws->sendBinary({ 0x00, 0xff, 0x10 }));
    REQUIRE(rec.waitFor([](auto& r) { return r.messages.size() == 2; }));

    CHECK(rec.messages[0] == "hello");
    CHECK(rec.text[0]);
    CHECK(rec.messages[1] == std::string("\x00\xff\x10", 3));
    CHECK_FALSE(rec.text[1]);

    ws->close();
    REQUIRE(rec.waitFor([](auto& r) { return r.closes.size() == 1; }));
    CHECK(rec.closes[0] == CloseCode::normal);
    CHECK_FALSE(ws->isConnected());
    CHECK(rec.errors.empty());

    CHECK(server.log->waitCloses(1));
    CHECK(server.log->closes[0] == CloseCode::normal);
}

TEST_CASE("Headers and subprotocols go into the upgrade", "[wsclient]")
{
    Server server;
    WebSocketClient client;
    Recorder rec;

    auto ws = client.connect({
        .url = server.url("/headers"),
        .headers = { { "X-Test", "42" } },
        .subprotocols = { "chat.v1", "chat.v2" },
    }, rec.callbacks());

    REQUIRE(rec.waitFor([](auto& r) { return r.messages.size() == 1; }));
    CHECK(rec.messages[0] == "chat.v1, chat.v2|42");
}

TEST_CASE("A rejected upgrade is an error and is not retried", "[wsclient][reconnect]")
{
    Server server;
    WebSocketClient client;

    SECTION("refused")
    {
        Recorder rec;
        auto ws = client.connect({ .url = server.url("/auth"), .reconnect = { .enabled = true, .initial_delay = 20ms } }, rec.callbacks());

        REQUIRE(rec.waitFor([](auto& r) { return !r.errors.empty(); }));
        CHECK(rec.errors[0] == client_error::upgrade_rejected);
        CHECK(ws->upgradeStatus() == 401);

        std::this_thread::sleep_for(200ms);
        std::lock_guard lock(rec.mutex);
        CHECK(rec.opens == 0);
        CHECK(rec.attempts.empty());
        CHECK(rec.errors.size() == 1);
    }

    SECTION("accepted with the right header")
    {
        Recorder rec;
        auto ws = client.connect({ .url = server.url("/auth"), .headers = { { "Authorization", "Bearer ok" } } }, rec.callbacks());
        CHECK(rec.waitFor([](auto& r) { return r.opens == 1; }));
        CHECK(ws->upgradeStatus() == 0);
    }
}

TEST_CASE("Reconnects after the server goes away, and works again", "[wsclient][reconnect]")
{
    WebSocketClient client;
    Recorder rec;
    std::shared_ptr<WebSocketClient::Connection> ws;
    unsigned short port;

    {
        Server first;
        port = first.port;
        ws = client.connect({ .url = first.url("/echo"), .reconnect = { .enabled = true, .initial_delay = 50ms, .max_delay = 200ms } }, rec.callbacks());
        REQUIRE(rec.waitFor([](auto& r) { return r.opens == 1; }));
    }   // stop(): the server closes with 1001

    REQUIRE(rec.waitFor([](auto& r) { return r.closes.size() == 1; }));
    CHECK(rec.closes[0] == CloseCode::going_away);
    REQUIRE(rec.waitFor([](auto& r) { return !r.attempts.empty(); }));

    Server second(false, port);
    REQUIRE(rec.waitFor([](auto& r) { return r.opens == 2; }, 5s));

    CHECK(ws->sendText("again"));
    REQUIRE(rec.waitFor([](auto& r) { return r.messages.size() == 1; }));
    CHECK(rec.messages[0] == "again");
}

TEST_CASE("Failed attempts back off, then give up", "[wsclient][reconnect]")
{
    WebSocketClient client;
    Recorder rec;

    // Nothing listens on port 1
    auto ws = client.connect({ .url = "ws://127.0.0.1:1/", .reconnect = { .enabled = true, .initial_delay = 20ms, .max_delay = 1s, .max_attempts = 3 } }, rec.callbacks());

    REQUIRE(rec.waitFor([](auto& r) { return !r.errors.empty() && r.errors.back() == client_error::gave_up; }, 5s));

    std::lock_guard lock(rec.mutex);
    CHECK(rec.attempts == std::vector<unsigned>{ 1, 2, 3 });
    CHECK(rec.errors.size() == 5);      // the first try, 3 retries, then gave_up
    for (std::size_t i = 0; i + 1 < rec.errors.size(); ++i)
        CHECK(rec.errors[i] == boost::asio::error::connection_refused);

    // Doubling, with at most +-20% jitter: each delay is clearly longer than the one before
    REQUIRE(rec.delays.size() == 3);
    CHECK(rec.delays[0] >= 16ms);
    CHECK(rec.delays[0] <= 24ms);
    CHECK(rec.delays[1] > rec.delays[0]);
    CHECK(rec.delays[2] > rec.delays[1]);
    CHECK(rec.opens == 0);
    CHECK(rec.closes.empty());
}

TEST_CASE("Without reconnect, a failed connect is only an error", "[wsclient]")
{
    WebSocketClient client;
    Recorder rec;

    auto ws = client.connect({ .url = "ws://127.0.0.1:1/" }, rec.callbacks());
    REQUIRE(rec.waitFor([](auto& r) { return !r.errors.empty(); }));
    std::this_thread::sleep_for(100ms);

    std::lock_guard lock(rec.mutex);
    CHECK(rec.errors.size() == 1);
    CHECK(rec.attempts.empty());
    CHECK_FALSE(ws->isConnected());
}

TEST_CASE("Bad URLs are reported with onError", "[wsclient]")
{
    WebSocketClient client;

    for (auto [url, expected] : { std::pair{ "not a url", client_error::invalid_url },
                                  std::pair{ "http://example.com/ws", client_error::unsupported_scheme },
                                  std::pair{ "ftp://example.com/", client_error::unsupported_scheme } })
    {
        Recorder rec;
        auto ws = client.connect({ .url = url, .reconnect = { .enabled = true } }, rec.callbacks());
        REQUIRE(rec.waitFor([](auto& r) { return !r.errors.empty(); }));
        CHECK(rec.errors[0] == expected);
        std::this_thread::sleep_for(50ms);
        std::lock_guard lock(rec.mutex);
        CHECK(rec.attempts.empty());
    }
}

TEST_CASE("The server's close code is reported", "[wsclient]")
{
    Server server;
    WebSocketClient client;
    Recorder rec;

    auto ws = client.connect({ .url = server.url("/policy") }, rec.callbacks());
    REQUIRE(rec.waitFor([](auto& r) { return r.opens == 1; }));

    ws->sendText("bye");
    REQUIRE(rec.waitFor([](auto& r) { return r.closes.size() == 1; }));
    CHECK(rec.closes[0] == CloseCode::policy_error);
    CHECK(rec.errors.empty());
}

TEST_CASE("close() never reconnects", "[wsclient][reconnect]")
{
    Server server;
    WebSocketClient client;
    Recorder rec;

    auto ws = client.connect({ .url = server.url("/echo"), .reconnect = { .enabled = true, .initial_delay = 20ms } }, rec.callbacks());
    REQUIRE(rec.waitFor([](auto& r) { return r.opens == 1; }));

    ws->close();
    REQUIRE(rec.waitFor([](auto& r) { return r.closes.size() == 1; }));
    std::this_thread::sleep_for(200ms);

    std::lock_guard lock(rec.mutex);
    CHECK(rec.opens == 1);
    CHECK(rec.attempts.empty());
    CHECK_FALSE(ws->isConnected());
    CHECK_FALSE(ws->sendText("too late"));
}

TEST_CASE("send() returns false when not connected or when the queue is full", "[wsclient]")
{
    Server server;
    WebSocketClient::Options options;
    options.send_queue_limit = 2;
    WebSocketClient client(options);
    Recorder rec;

    auto ws = client.connect({ .url = server.url("/echo") }, rec.callbacks());
    REQUIRE(rec.waitFor([](auto& r) { return r.opens == 1; }));

    // Far faster than the socket can take: the queue fills up
    auto payload = std::make_shared<const std::string>(256 * 1024, 'x');
    int refused = 0;
    std::size_t max_pending = 0;
    for (int i = 0; i < 200; ++i)
    {
        if (!ws->sendText(payload))
            ++refused;
        max_pending = std::max(max_pending, ws->pendingMessages());
    }

    CHECK(refused > 0);
    CHECK(max_pending <= 2);
    CHECK(ws->isConnected());       // a full queue doesn't close the connection
}

TEST_CASE("Dropping the last handle closes the connection", "[wsclient]")
{
    Server server;
    WebSocketClient client;
    Recorder rec;

    auto ws = client.connect({ .url = server.url("/echo"), .reconnect = { .enabled = true } }, rec.callbacks());
    REQUIRE(rec.waitFor([](auto& r) { return r.opens == 1; }));

    ws.reset();
    REQUIRE(server.log->waitCloses(1));
    CHECK(server.log->closes[0] == CloseCode::normal);
    REQUIRE(rec.waitFor([](auto& r) { return r.closes.size() == 1; }));
}

TEST_CASE("Destroying the client closes its connections with 1001", "[wsclient]")
{
    Server server;
    Recorder rec;
    std::shared_ptr<WebSocketClient::Connection> ws;      // outlives the client: its own close() must not come first

    {
        WebSocketClient client;
        ws = client.connect({ .url = server.url("/echo"), .reconnect = { .enabled = true } }, rec.callbacks());
        REQUIRE(rec.waitFor([](auto& r) { return r.opens == 1; }));
    }

    REQUIRE(server.log->waitCloses(1));
    CHECK(server.log->closes[0] == CloseCode::going_away);
}

TEST_CASE("WSS", "[wsclient][tls]")
{
    Server server(true);

    SECTION("trusted CA")
    {
        WebSocketClient client(trustingTestCa());
        Recorder rec;
        auto ws = client.connect({ .url = server.url("/echo", "wss", "localhost") }, rec.callbacks());
        REQUIRE(rec.waitFor([](auto& r) { return r.opens == 1; }));

        const std::string big(300 * 1024, 'w');
        ws->sendText(big);
        REQUIRE(rec.waitFor([](auto& r) { return r.messages.size() == 1; }));
        CHECK(rec.messages[0] == big);
    }

    SECTION("unknown CA or wrong host name: TLS error, never opened")
    {
        WebSocketClient bundled;
        WebSocketClient trusting(trustingTestCa());

        for (auto [client, host] : { std::pair{ &bundled, "localhost" }, std::pair{ &trusting, "127.0.0.1" } })
        {
            Recorder rec;
            auto ws = client->connect({ .url = server.url("/echo", "wss", host) }, rec.callbacks());
            REQUIRE(rec.waitFor([](auto& r) { return !r.errors.empty(); }));
            CHECK(rec.errors[0].category() == boost::asio::error::get_ssl_category());
            CHECK(rec.opens == 0);
        }
    }
}

TEST_CASE("Callbacks can run on a given executor, with their own copy of messages", "[wsclient]")
{
    Server server;
    WebSocketClient client;
    Recorder rec;

    boost::asio::io_context app;
    auto guard = boost::asio::make_work_guard(app);
    std::thread::id app_thread;
    std::thread runner([&] { app_thread = std::this_thread::get_id(); app.run(); });

    auto callbacks = rec.callbacks();
    callbacks.executor = app.get_executor();
    auto ws = client.connect({ .url = server.url("/echo") }, callbacks);

    REQUIRE(rec.waitFor([](auto& r) { return r.opens == 1; }));
    for (int i = 0; i < 20; ++i)
        ws->sendText("msg " + std::to_string(i));
    REQUIRE(rec.waitFor([](auto& r) { return r.messages.size() == 20; }));

    ws->close();
    REQUIRE(rec.waitFor([](auto& r) { return r.closes.size() == 1; }));
    guard.reset();
    runner.join();

    for (int i = 0; i < 20; ++i)
        CHECK(rec.messages[i] == "msg " + std::to_string(i));
    for (auto id : rec.threads)
        CHECK(id == app_thread);
}

// Needs the internet, so hidden: ./test_websocket_client "[network]"
TEST_CASE("A public WSS echo server, verified with the bundled Mozilla CAs", "[.][network]")
{
    WebSocketClient client;
    Recorder rec;

    auto ws = client.connect({ .url = "wss://echo.websocket.org/" }, rec.callbacks());
    REQUIRE(rec.waitFor([](auto& r) { return r.opens == 1 || !r.errors.empty(); }, 15s));
    INFO("error: " << (rec.errors.empty() ? std::string("none") : rec.errors[0].message()));
    REQUIRE(rec.opens == 1);

    ws->sendText("hello from cppgate");
    // This server greets first, then echoes
    REQUIRE(rec.waitFor([](auto& r) {
        return std::find(r.messages.begin(), r.messages.end(), "hello from cppgate") != r.messages.end();
    }, 15s));

    ws->close();
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE(rec.waitFor([](auto& r) { return r.closes.size() == 1; }, 15s));
    INFO("close took " << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() << " ms");
    CHECK(rec.closes[0] == CloseCode::normal);
}


TEST_CASE("close() while the server is still sending is clean and quick", "[wsclient]")
{
    for (bool tls : { false, true })
    {
        Server server(tls);
        WebSocketClient client(trustingTestCa());
        Recorder rec;

        // Close as soon as it opens: the server's messages arrive during our close
        auto callbacks = rec.callbacks();
        auto record_open = callbacks.onOpen;
        callbacks.onOpen = [record_open](WebSocketClient::Connection& ws) { record_open(ws); ws.close(); };

        auto t0 = std::chrono::steady_clock::now();
        auto ws = client.connect({ .url = tls ? server.url("/greet", "wss", "localhost") : server.url("/greet") }, callbacks);

        INFO((tls ? "wss" : "ws"));
        REQUIRE(rec.waitFor([](auto& r) { return r.closes.size() == 1; }, 5s));
        CHECK(std::chrono::steady_clock::now() - t0 < 1s);       // not the 5 s close_timeout
        CHECK(rec.closes[0] == CloseCode::normal);
        CHECK(rec.errors.empty());

        REQUIRE(server.log->waitCloses(1));
        CHECK(server.log->closes[0] == CloseCode::normal);
    }
}

namespace {

    // A WSS client like some real ones: it answers close frames, but never sends TLS close_notify
    // and keeps TCP open, waiting for the server to close it
    struct RudeTlsClient
    {
        boost::asio::io_context ioc;
        boost::asio::ssl::context ctx{ boost::asio::ssl::context::tls_client };
        boost::asio::ssl::stream<tcp::socket> stream{ ioc, ctx };
        std::string pending;

        RudeTlsClient(unsigned short port, const std::string& path)
        {
            ctx.set_verify_mode(boost::asio::ssl::verify_none);
            stream.next_layer().connect({ boost::asio::ip::make_address("127.0.0.1"), port });

            #ifndef _WIN32
                // A server that never answers fails the test instead of hanging it
                timeval tv{ 5, 0 };
                setsockopt(stream.next_layer().native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            #endif

            stream.handshake(boost::asio::ssl::stream_base::client);
            boost::asio::write(stream, boost::asio::buffer(
                "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"));
            while (pending.find("\r\n\r\n") == std::string::npos)
                readMore();
            pending.erase(0, pending.find("\r\n\r\n") + 4);
        }

        void readMore()
        {
            char buffer[4096];
            auto n = stream.read_some(boost::asio::buffer(buffer));
            pending.append(buffer, n);
        }

        void send(std::uint8_t opcode, const std::string& payload)
        {
            std::string frame{ static_cast<char>(0x80 | opcode), static_cast<char>(0x80 | payload.size()) };
            const char mask[4] = { 1, 2, 3, 4 };
            frame.append(mask, 4);
            for (std::size_t i = 0; i < payload.size(); ++i)
                frame += static_cast<char>(payload[i] ^ mask[i % 4]);
            boost::asio::write(stream, boost::asio::buffer(frame));
        }

        void sendClose(std::uint16_t code)
        {
            send(0x8, std::string{ static_cast<char>(code >> 8), static_cast<char>(code & 0xff) });
        }

        // Skips other frames until a close frame from the server; returns its code
        std::uint16_t readClose()
        {
            for (;;)
            {
                while (pending.size() >= 2)
                {
                    auto opcode = static_cast<std::uint8_t>(pending[0]) & 0x0f;
                    std::size_t length = static_cast<std::uint8_t>(pending[1]) & 0x7f;
                    if (pending.size() < 2 + length)
                        break;
                    std::string payload = pending.substr(2, length);
                    pending.erase(0, 2 + length);
                    if (opcode == 0x8)
                        return payload.size() >= 2 ? static_cast<std::uint16_t>((static_cast<std::uint8_t>(payload[0]) << 8) | static_cast<std::uint8_t>(payload[1])) : 0;
                }
                readMore();
            }
        }
    };

}

TEST_CASE("The server finishes a WSS close quickly with a client that never sends close_notify", "[wsserver][tls]")
{
    Server server(true);

    SECTION("the client starts the close")
    {
        RudeTlsClient client(server.port, "/echo");
        client.sendClose(1000);
        CHECK(client.readClose() == 1000);

        // The client now stays silent with TCP open: the server must not wait for its close_notify
        auto t0 = std::chrono::steady_clock::now();
        REQUIRE(server.log->waitCloses(1, 3s));
        CHECK(std::chrono::steady_clock::now() - t0 < 2s);
        CHECK(server.log->closes[0] == CloseCode::normal);
    }

    SECTION("the server starts the close")
    {
        RudeTlsClient client(server.port, "/policy");
        client.send(0x1, "bye");                    // the route closes with 1008
        CHECK(client.readClose() == 1008);
        client.sendClose(1008);

        auto t0 = std::chrono::steady_clock::now();
        REQUIRE(server.log->waitCloses(1, 3s));
        CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    }
}
