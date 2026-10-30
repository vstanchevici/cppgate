#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cppgate/Listener.h>

#include <boost/asio/ssl.hpp>

#include "../http_client/TestCertificates.h"

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

// What the server answers to oversized and malformed requests, over HTTP and HTTPS:
// a real status (400, 413, 414, 431), then it closes the connection.

namespace {

    constexpr std::size_t body_limit = 64 * 1024;

    void initLoggerOnce()
    {
        static std::once_flag once;
        std::call_once(once, [] { gtvr::Logger::Init(); });
    }

    // An in-process cppgate server: GET /hello, body limit 64 KB, HTTP and HTTPS on one port
    struct Server
    {
        boost::asio::io_context ioc;
        boost::asio::ssl::context tls{ boost::asio::ssl::context::tls_server };
        std::shared_ptr<gtvr::Listener> listener;
        std::future<void> running;

        Server()
        {
            initLoggerOnce();

            auto router = std::make_shared<gtvr::router::Router>();
            router->add({ http::verb::get }, "/hello", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
                res.send("hello", "text/plain");
            });

            tls.use_certificate_chain(boost::asio::buffer(std::string_view(test_certificates::server_cert)));
            tls.use_private_key(boost::asio::buffer(std::string_view(test_certificates::server_key)), boost::asio::ssl::context::pem);

            gtvr::ServerOptions options;
            options.http_body_limit = body_limit;
            options.http_linger_timeout = 200ms;        // after an error response; keeps the test fast

            listener = std::make_shared<gtvr::Listener>(ioc, tcp::endpoint{ boost::asio::ip::make_address("127.0.0.1"), 0 });
            listener->setRouter(router);
            listener->setOptions(options);
            listener->setSSLContext(&tls);
            listener->run<gtvr::Protocol::HTTP | gtvr::Protocol::HTTPS>();
            running = std::async(std::launch::async, [this] { ioc.run(); });
        }

        unsigned short port() const { return listener->localEndpoint().port(); }

        ~Server()
        {
            listener->stop();
            if (running.wait_for(5s) != std::future_status::ready)
                ioc.stop();
            running.wait();
        }
    };

    // A raw HTTP client, plain or TLS, that writes bytes as given and reads what comes back.
    // Every operation is asynchronous with a deadline (io_context::run_for), so a server that
    // doesn't answer fails the test instead of hanging it, the same way over TCP and TLS.
    class RawClient
    {
        private:
            boost::asio::io_context ioc_;
            boost::asio::ssl::context ctx_{ boost::asio::ssl::context::tls_client };
            std::optional<tcp::socket> plain_;
            std::optional<boost::asio::ssl::stream<tcp::socket>> tls_;

            tcp::socket& socket() { return plain_ ? *plain_ : tls_->next_layer(); }

            // Runs one operation; returns its error, or timed_out after cancelling it
            template<typename Start>
            boost::system::error_code run(std::chrono::milliseconds timeout, Start&& start, std::size_t* transferred = nullptr)
            {
                std::optional<boost::system::error_code> result;
                std::size_t bytes = 0;
                start([&](boost::system::error_code ec, std::size_t n) { result = ec; bytes = n; });

                ioc_.restart();
                ioc_.run_for(timeout);
                if (!result)
                {
                    boost::system::error_code ignored;
                    socket().cancel(ignored);
                    ioc_.restart();
                    ioc_.run();
                    result = boost::asio::error::timed_out;
                }
                if (transferred)
                    *transferred = bytes;
                return *result;
            }

        public:
            struct Read
            {
                std::string data;
                bool closed = false;        // the server ended the connection (EOF, reset, TLS end)

                std::string statusLine() const { return data.substr(0, data.find("\r\n")); }
                bool status(int code) const { return statusLine().rfind("HTTP/1.1 " + std::to_string(code), 0) == 0; }

                std::size_t count(const std::string& what) const
                {
                    std::size_t n = 0;
                    for (auto pos = data.find(what); pos != std::string::npos; pos = data.find(what, pos + 1))
                        ++n;
                    return n;
                }
            };

            RawClient(unsigned short port, bool tls)
            {
                tcp::endpoint endpoint{ boost::asio::ip::make_address("127.0.0.1"), port };
                if (tls)
                {
                    ctx_.set_verify_mode(boost::asio::ssl::verify_none);
                    tls_.emplace(ioc_, ctx_);
                    tls_->next_layer().connect(endpoint);
                    auto ec = run(5s, [&](auto handler) {
                        tls_->async_handshake(boost::asio::ssl::stream_base::client, [handler](boost::system::error_code ec) mutable { handler(ec, 0); });
                    });
                    REQUIRE_FALSE(ec);
                }
                else
                {
                    plain_.emplace(ioc_);
                    plain_->connect(endpoint);
                }
            }

            void write(const std::string& bytes)
            {
                auto ec = run(30s, [&](auto handler) {
                    if (plain_)
                        boost::asio::async_write(*plain_, boost::asio::buffer(bytes), handler);
                    else
                        boost::asio::async_write(*tls_, boost::asio::buffer(bytes), handler);
                });
                REQUIRE_FALSE(ec);
            }

            // Reads until done(data) is true, the server closes, or the timeout (all together)
            template<typename Done>
            Read read(std::chrono::milliseconds timeout, Done&& done)
            {
                Read result;
                auto deadline = std::chrono::steady_clock::now() + timeout;
                char buffer[16384];

                while (!done(result))
                {
                    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                    if (left <= 0ms)
                        return result;

                    std::size_t n = 0;
                    auto ec = run(left, [&](auto handler) {
                        if (plain_)
                            plain_->async_read_some(boost::asio::buffer(buffer), handler);
                        else
                            tls_->async_read_some(boost::asio::buffer(buffer), handler);
                    }, &n);
                    result.data.append(buffer, n);

                    if (ec == boost::asio::error::timed_out)
                        return result;
                    if (ec)
                    {
                        result.closed = true;
                        return result;
                    }
                }
                return result;
            }

            // Until the server closes, or the timeout
            Read read(std::chrono::milliseconds timeout)
            {
                return read(timeout, [](const Read&) { return false; });
            }

            Read readUntil(std::chrono::milliseconds timeout, const std::string& what)
            {
                return read(timeout, [&](const Read& r) { return r.data.find(what) != std::string::npos; });
            }

            void close()
            {
                boost::system::error_code ignored;
                socket().close(ignored);
            }
    };

    std::string get(const std::string& target, const std::string& extra_headers = "")
    {
        return "GET " + target + " HTTP/1.1\r\nHost: x\r\n" + extra_headers + "\r\n";
    }

    std::string post(std::size_t size)
    {
        return "POST /hello HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(size) + "\r\n\r\n" + std::string(size, 'b');
    }

    // Sends a request that must be refused: returns what came back, the connection must be closed after it
    RawClient::Read refused(unsigned short port, bool tls, const std::string& request)
    {
        RawClient client(port, tls);
        client.write(request);
        return client.read(5s);
    }

}

TEST_CASE("Request size limits and malformed requests", "[limits]")
{
    bool tls = GENERATE(false, true);
    Server server;
    auto port = server.port();

    DYNAMIC_SECTION((tls ? "https" : "http") << ": URLs")
    {
        // 7 KB fits in the 8 KB header limit: served, and the connection stays open (keep-alive)
        RawClient client(port, tls);
        client.write(get("/hello?pad=" + std::string(7 * 1024, 'x')));
        auto ok = client.readUntil(3s, "hello");
        CHECK(ok.status(200));
        CHECK_FALSE(client.read(300ms).closed);

        for (std::size_t kb : { 9, 64 })
        {
            INFO(kb << " KB URL");
            auto r = refused(port, tls, get("/hello?pad=" + std::string(kb * 1024, 'x')));
            CHECK(r.status(414));
            CHECK(r.closed);
        }
    }

    DYNAMIC_SECTION((tls ? "https" : "http") << ": headers")
    {
        auto one_big = refused(port, tls, get("/hello", "X-Big: " + std::string(9 * 1024, 'h') + "\r\n"));
        CHECK(one_big.status(431));
        CHECK(one_big.closed);

        std::string many;
        for (int i = 0; i < 100; ++i)
            many += "X-H" + std::to_string(i) + ": " + std::string(80, 'v') + "\r\n";
        auto many_small = refused(port, tls, get("/hello", many));
        CHECK(many_small.status(431));
        CHECK(many_small.closed);
    }

    DYNAMIC_SECTION((tls ? "https" : "http") << ": bodies")
    {
        // At the limit: routed normally (/hello is GET only: 405), connection kept
        {
            RawClient client(port, tls);
            client.write(post(body_limit));
            auto r = client.readUntil(3s, "\r\n\r\n");
            CHECK(r.status(405));
            CHECK_FALSE(client.read(300ms).closed);
        }

        // Over it, even far over: 413 is received (no reset while the client is still sending), then closed
        for (std::size_t size : { body_limit + 1, std::size_t(1024 * 1024), std::size_t(10 * 1024 * 1024) })
        {
            INFO(size << " byte body");
            auto r = refused(port, tls, post(size));
            CHECK(r.status(413));
            CHECK(r.closed);
        }
    }

    DYNAMIC_SECTION((tls ? "https" : "http") << ": malformed requests")
    {
        auto garbage = refused(port, tls, "HELLO WORLD\r\n\r\n");
        CHECK(garbage.status(400));
        CHECK(garbage.closed);

        auto bad_header = refused(port, tls, "GET /hello HTTP/1.1\r\nno colon here\r\n\r\n");
        CHECK(bad_header.status(400));
        CHECK(bad_header.closed);
    }

    DYNAMIC_SECTION((tls ? "https" : "http") << ": keep-alive and clients that leave")
    {
        // Two requests in one write, on one connection: two responses
        {
            RawClient client(port, tls);
            client.write(get("/hello") + get("/hello"));
            auto r = client.read(3s, [](const RawClient::Read& r) { return r.count("HTTP/1.1 200") >= 2; });
            CHECK(r.count("HTTP/1.1 200") == 2);
        }

        // A client that leaves in the middle of a request doesn't disturb the next one
        {
            RawClient client(port, tls);
            client.write("GET /hel");
            client.close();
        }
        std::this_thread::sleep_for(100ms);

        RawClient next(port, tls);
        next.write(get("/hello", "Connection: close\r\n"));
        CHECK(next.read(3s).status(200));
    }
}
