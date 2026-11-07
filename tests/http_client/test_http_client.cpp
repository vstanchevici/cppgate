#include <catch2/catch_test_macros.hpp>

#include <cppgate/Client/HttpClient.h>
#include <cppgate/Listener.h>

#include <boost/asio/ssl.hpp>

#include "TestCertificates.h"

#include <openssl/pem.h>
#include <openssl/x509.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;
using gtvr::HttpClient;
using gtvr::client_error;

namespace {

    void initLoggerOnce()
    {
        static std::once_flag once;
        std::call_once(once, [] { gtvr::Logger::Init(); });
    }

    // An in-process cppgate server, plain or HTTP+HTTPS on one port
    struct Server
    {
        boost::asio::io_context ioc;
        boost::asio::ssl::context tls{ boost::asio::ssl::context::tls_server };
        std::shared_ptr<gtvr::router::Router> router = std::make_shared<gtvr::router::Router>();
        std::shared_ptr<gtvr::Listener> listener;
        std::future<void> running;
        unsigned short port = 0;

        explicit Server(bool with_tls = false, unsigned short fixed_port = 0)
        {
            initLoggerOnce();

            router->add({ http::verb::get }, "/hello", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
                res.send("hello", "text/plain");
            });
            router->add({ http::verb::get }, "/status/404", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
                res.sendNotFound("nope");
            });
            router->add({ http::verb::post, http::verb::put }, "/echo", [](gtvr::router::HttpRequest& req, gtvr::router::HttpResponse& res) {
                res.sendContent(req.body());
            });
            router->add({ http::verb::get }, "/big", [](gtvr::router::HttpRequest& req, gtvr::router::HttpResponse& res) {
                res.sendContent(std::string(std::stoul(req.urlQueries().get("size")), 'x'));
            });
            router->add({ http::verb::get }, "/slow", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
                std::this_thread::sleep_for(500ms);
                res.sendContent("slow");
            });

            const std::initializer_list<http::verb> any_method = { http::verb::get, http::verb::head, http::verb::post, http::verb::put, http::verb::delete_ };

            // /redirect/302?to=/inspect
            router->add(any_method, "/redirect/{code}", [](gtvr::router::HttpRequest& req, gtvr::router::HttpResponse& res) {
                res.header(http::field::location, req.urlQueries().get("to"));
                res.send(static_cast<http::status>(std::stoi(std::string(req.urlParam("code")))), "moved");
            });
            router->add({ http::verb::get }, "/no-location", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
                res.send(http::status::found, "moved");
            });
            router->add({ http::verb::get }, "/loop", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
                res.header(http::field::location, "/loop");
                res.send(http::status::found, "moved");
            });
            router->add({ http::verb::get }, "/slow-redirect", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
                std::this_thread::sleep_for(300ms);
                res.header(http::field::location, "/slow");
                res.send(http::status::found, "moved");
            });

            // Relative to the current URL, then to the host, then upwards
            router->add({ http::verb::get }, "/chain/one", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
                res.header(http::field::location, "two");
                res.send(http::status::found, "moved");
            });
            router->add({ http::verb::get }, "/chain/two", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
                res.header(http::field::location, "/chain/three");
                res.send(http::status::moved_permanently, "moved");
            });
            router->add({ http::verb::get }, "/chain/three", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
                res.header(http::field::location, "../hello");
                res.send(http::status::see_other, std::string(256 * 1024, 'm'));
            });

            // method|body|content-type|authorization|cookie|host
            router->add(any_method, "/inspect", [](gtvr::router::HttpRequest& req, gtvr::router::HttpResponse& res) {
                auto headers = req.headers();
                std::string seen = std::string(req.method_string()) + "|" + std::string(req.body())
                    + "|" + std::string(headers.get(http::field::content_type))
                    + "|" + std::string(headers.get(http::field::authorization))
                    + "|" + std::string(headers.get(http::field::cookie))
                    + "|" + std::string(headers.get(http::field::host));
                res.sendContent(seen);
            });

            listener = std::make_shared<gtvr::Listener>(ioc, tcp::endpoint{ boost::asio::ip::make_address("127.0.0.1"), fixed_port });
            listener->setRouter(router);

            gtvr::ServerOptions options;
            options.http_body_limit = 4 * 1024 * 1024;
            listener->setOptions(options);

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

        std::string url(const std::string& path, const char* scheme = "http", const char* host = "127.0.0.1") const
        {
            return std::string(scheme) + "://" + host + ":" + std::to_string(port) + path;
        }

        void stop()
        {
            if (!listener)
                return;
            listener->stop();
            if (running.wait_for(3s) != std::future_status::ready)
                ioc.stop();
            running.wait();
            listener.reset();
        }

        ~Server()
        {
            stop();
        }
    };

    // A folder of our own under the temp dir, removed at the end of the test
    struct TempDir
    {
        std::filesystem::path path;

        TempDir()
        {
            path = std::filesystem::temp_directory_path() / ("cppgate_client_test_" + std::to_string(std::random_device{}()));
            std::filesystem::create_directories(path);
        }

        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        std::string write(const std::string& name, const std::string& content) const
        {
            auto file = path / name;
            std::ofstream(file) << content;
            return file.string();
        }
    };

    // The file name OpenSSL looks for in a hashed CA directory: <subject hash>.0
    std::string hashedName(const char* pem)
    {
        BIO* bio = BIO_new_mem_buf(pem, -1);
        X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        char name[32];
        std::snprintf(name, sizeof(name), "%08lx.0", X509_subject_name_hash(cert));
        X509_free(cert);
        BIO_free(bio);
        return name;
    }

    HttpClient::Options trustingTestCa()
    {
        HttpClient::Options options;
        options.use_bundled_cas = false;
        options.extra_ca_pem = { test_certificates::ca };
        return options;
    }

}

TEST_CASE("GET returns the response", "[client]")
{
    Server server;
    HttpClient client;

    std::atomic<int> completions{ 0 };
    auto call = client.send({ .url = server.url("/hello") }, {
        .onComplete = [&](HttpClient::Result) { ++completions; },
    });

    auto result = call.wait();
    REQUIRE(result);
    CHECK(result->status() == 200);
    CHECK(result->body() == "hello");
    CHECK(result->header("Content-Type") == "text/plain");

    std::this_thread::sleep_for(50ms);
    CHECK(completions == 1);
    CHECK(call.done());
}

TEST_CASE("HTTP errors are responses, not failures", "[client]")
{
    Server server;
    HttpClient client;

    auto result = client.send({ .url = server.url("/status/404") }).wait();
    REQUIRE(result);
    CHECK(result->status() == 404);
    CHECK(result->body() == "nope");
}

TEST_CASE("Upload and download progress", "[client][progress]")
{
    Server server;
    HttpClient client;

    const std::string body(1024 * 1024, 'u');
    std::mutex mutex;
    std::vector<HttpClient::Progress> events;

    auto result = client.send({ .method = http::verb::post, .url = server.url("/echo"), .body = body }, {
        .onProgress = [&](const HttpClient::Progress& p) { std::lock_guard lock(mutex); events.push_back(p); },
    }).wait();

    REQUIRE(result);
    CHECK(result->body().size() == body.size());

    std::lock_guard lock(mutex);
    std::vector<HttpClient::Progress> up, down;
    for (auto& e : events)
        (e.direction == HttpClient::Progress::Direction::upload ? up : down).push_back(e);

    // Every upload event comes before every download event
    REQUIRE(!up.empty());
    REQUIRE(!down.empty());
    auto first_download = std::find_if(events.begin(), events.end(), [](auto& e) { return e.direction == HttpClient::Progress::Direction::download; });
    CHECK(std::all_of(first_download, events.end(), [](auto& e) { return e.direction == HttpClient::Progress::Direction::download; }));

    // Several events for 1 MB, growing, the last one complete
    CHECK(up.size() >= 2);
    for (std::size_t i = 1; i < up.size(); ++i)
        CHECK(up[i].transferred >= up[i - 1].transferred);
    CHECK(up.back().transferred == body.size());
    CHECK(up.back().total == body.size());

    CHECK(down.back().transferred == body.size());
    CHECK(down.back().total == body.size());
}

TEST_CASE("No upload progress without a body", "[client][progress]")
{
    Server server;
    HttpClient client;

    std::atomic<int> uploads{ 0 }, downloads{ 0 };
    auto result = client.send({ .url = server.url("/hello") }, {
        .onProgress = [&](const HttpClient::Progress& p) { ++(p.direction == HttpClient::Progress::Direction::upload ? uploads : downloads); },
    }).wait();

    REQUIRE(result);
    CHECK(uploads == 0);
    CHECK(downloads >= 1);
}

TEST_CASE("Redirects are responses by default", "[client][redirect]")
{
    Server server;
    HttpClient client;

    auto result = client.send({ .url = server.url("/redirect/302?to=/hello") }).wait();
    REQUIRE(result);
    CHECK(result->status() == 302);
    CHECK(result->header("Location") == "/hello");
    CHECK(result->body() == "moved");
}

TEST_CASE("Redirects are followed with max_redirects", "[client][redirect]")
{
    Server server;
    HttpClient::Options options;
    options.max_redirects = 3;
    HttpClient client(options);

    SECTION("relative Locations, up to the limit")
    {
        auto result = client.send({ .url = server.url("/chain/one") }).wait();
        REQUIRE(result);
        CHECK(result->status() == 200);
        CHECK(result->body() == "hello");
    }

    SECTION("an absolute Location")
    {
        auto result = client.send({ .url = server.url("/redirect/302?to=" + server.url("/hello")) }).wait();
        REQUIRE(result);
        CHECK(result->body() == "hello");
    }

    SECTION("over the limit fails")
    {
        auto result = client.send({ .url = server.url("/loop") }).wait();
        REQUIRE_FALSE(result);
        CHECK(result.error() == client_error::too_many_redirects);
    }

    SECTION("a 3xx that isn't a redirect, or has no Location, is the response")
    {
        auto multiple = client.send({ .url = server.url("/redirect/300?to=/hello") }).wait();
        REQUIRE(multiple);
        CHECK(multiple->status() == 300);

        auto no_location = client.send({ .url = server.url("/no-location") }).wait();
        REQUIRE(no_location);
        CHECK(no_location->status() == 302);
    }

    SECTION("a Location that can't be followed fails")
    {
        auto scheme = client.send({ .url = server.url("/redirect/302?to=ftp://example.com/file") }).wait();
        REQUIRE_FALSE(scheme);
        CHECK(scheme.error() == client_error::unsupported_scheme);

        auto invalid = client.send({ .url = server.url("/redirect/302?to=http://") }).wait();
        REQUIRE_FALSE(invalid);
        CHECK(invalid.error() == client_error::invalid_url);
    }

    SECTION("connections are given back on the way")
    {
        REQUIRE(client.send({ .url = server.url("/chain/one") }).wait());
        CHECK(client.idleConnections() == 1);
    }
}

TEST_CASE("Redirects change or keep the method and the body", "[client][redirect]")
{
    Server server;
    HttpClient::Options options;
    options.max_redirects = 1;
    HttpClient client(options);

    const std::vector<std::pair<std::string, std::string>> headers = { { "Content-Type", "text/plain" } };

    auto send = [&](http::verb method, const std::string& code, const std::string& body) {
        auto result = client.send({ .method = method, .url = server.url("/redirect/" + code + "?to=/inspect"), .headers = headers, .body = body }).wait();
        REQUIRE(result);
        CHECK(result->status() == 200);
        return result->body();
    };

    const std::string host = "127.0.0.1:" + std::to_string(server.port);
    const std::string as_get = "GET|||||" + host;

    SECTION("301, 302 and 303 turn POST into GET, without the body and its headers")
    {
        CHECK(send(http::verb::post, "301", "data") == as_get);
        CHECK(send(http::verb::post, "302", "data") == as_get);
        CHECK(send(http::verb::post, "303", "data") == as_get);
    }

    SECTION("307 and 308 keep the method and the body")
    {
        CHECK(send(http::verb::post, "307", "data") == "POST|data|text/plain|||" + host);
        CHECK(send(http::verb::post, "308", "data") == "POST|data|text/plain|||" + host);
        CHECK(send(http::verb::put, "307", "data") == "PUT|data|text/plain|||" + host);
    }

    SECTION("301 and 302 keep other methods, 303 doesn't")
    {
        CHECK(send(http::verb::put, "302", "data") == "PUT|data|text/plain|||" + host);
        CHECK(send(http::verb::delete_, "301", "") == "DELETE||text/plain|||" + host);
        CHECK(send(http::verb::put, "303", "data") == as_get);
    }

    SECTION("HEAD stays HEAD")
    {
        auto result = client.send({ .method = http::verb::head, .url = server.url("/redirect/303?to=/inspect") }).wait();
        REQUIRE(result);
        CHECK(result->status() == 200);
        CHECK(result->body().empty());
    }
}

TEST_CASE("Redirects to another origin drop the credentials", "[client][redirect]")
{
    Server server;
    Server other;
    HttpClient::Options options;
    options.max_redirects = 1;
    HttpClient client(options);

    const std::vector<std::pair<std::string, std::string>> headers = {
        { "Authorization", "Bearer secret" }, { "Cookie", "session=secret" }, { "X-Other", "kept" } };

    SECTION("the same origin keeps them")
    {
        auto result = client.send({ .url = server.url("/redirect/302?to=/inspect"), .headers = headers }).wait();
        REQUIRE(result);
        CHECK(result->body() == "GET|||Bearer secret|session=secret|127.0.0.1:" + std::to_string(server.port));
    }

    SECTION("another port doesn't, and Host follows")
    {
        auto result = client.send({ .url = server.url("/redirect/302?to=" + other.url("/inspect")), .headers = headers }).wait();
        REQUIRE(result);
        CHECK(result->body() == "GET|||||127.0.0.1:" + std::to_string(other.port));
    }

    SECTION("another host doesn't, even on the same port")
    {
        auto result = client.send({ .url = server.url("/redirect/302?to=" + server.url("/inspect", "http", "localhost")), .headers = headers }).wait();
        REQUIRE(result);
        CHECK(result->body() == "GET|||||localhost:" + std::to_string(server.port));
    }

    SECTION("a custom Host doesn't follow to another origin")
    {
        auto result = client.send({ .url = server.url("/redirect/302?to=" + other.url("/inspect")), .headers = { { "Host", "custom.example" } } }).wait();
        REQUIRE(result);
        CHECK(result->body() == "GET|||||127.0.0.1:" + std::to_string(other.port));
    }

    SECTION("a kept body goes with the method, without the credentials")
    {
        auto result = client.send({ .method = http::verb::post, .url = server.url("/redirect/307?to=" + other.url("/inspect")), .headers = headers, .body = "data" }).wait();
        REQUIRE(result);
        CHECK(result->body() == "POST|data||||127.0.0.1:" + std::to_string(other.port));
    }
}

TEST_CASE("Redirects between HTTP and HTTPS", "[client][redirect][tls]")
{
    Server server(true);
    auto options = trustingTestCa();
    options.max_redirects = 1;
    HttpClient client(options);

    SECTION("http to https is followed")
    {
        auto target = server.url("/hello", "https", "localhost");
        auto result = client.send({ .url = server.url("/redirect/302?to=" + target, "http", "localhost") }).wait();
        REQUIRE(result);
        CHECK(result->body() == "hello");
    }

    SECTION("https to http is refused")
    {
        auto target = server.url("/hello", "http", "localhost");
        auto result = client.send({ .url = server.url("/redirect/302?to=" + target, "https", "localhost") }).wait();
        REQUIRE_FALSE(result);
        CHECK(result.error() == client_error::insecure_redirect);
    }
}

TEST_CASE("The request timeout covers the redirects", "[client][redirect][timeout]")
{
    Server server;
    HttpClient::Options options;
    options.max_redirects = 1;
    HttpClient client(options);

    // 300 ms + 500 ms: each one fits, both don't
    auto result = client.send({ .url = server.url("/slow-redirect"), .timeout = 600ms }).wait();
    REQUIRE_FALSE(result);
    CHECK(result.error() == client_error::timeout);
}

TEST_CASE("Redirects and progress", "[client][redirect][progress]")
{
    Server server;
    HttpClient::Options options;
    options.max_redirects = 3;
    HttpClient client(options);

    std::mutex mutex;
    std::vector<HttpClient::Progress> events;
    auto onProgress = [&](const HttpClient::Progress& p) {
        std::lock_guard lock(mutex);
        events.push_back(p);
    };

    SECTION("the redirect responses have no download events")
    {
        // /chain/three has a 256 KB body
        auto result = client.send({ .url = server.url("/chain/one") }, { .onProgress = onProgress }).wait();
        REQUIRE(result);
        std::this_thread::sleep_for(50ms);

        std::lock_guard lock(mutex);
        REQUIRE_FALSE(events.empty());
        for (const auto& p : events)
        {
            CHECK(p.direction == HttpClient::Progress::Direction::download);
            CHECK(p.transferred <= 5);          // "hello"
        }
        CHECK(events.back().transferred == 5);
    }

    SECTION("a kept body uploads again, from 0")
    {
        const std::string body(128 * 1024, 'u');
        auto result = client.send({ .method = http::verb::post, .url = server.url("/redirect/307?to=/echo"), .body = body }, { .onProgress = onProgress }).wait();
        REQUIRE(result);
        CHECK(result->body() == body);
        std::this_thread::sleep_for(50ms);

        std::lock_guard lock(mutex);
        std::size_t last_upload_events = 0;
        for (const auto& p : events)
            if (p.direction == HttpClient::Progress::Direction::upload && p.transferred == body.size())
                ++last_upload_events;
        CHECK(last_upload_events >= 2);
    }
}

TEST_CASE("Connections are kept alive and reused", "[client][keepalive]")
{
    Server server;
    HttpClient client;

    for (int i = 0; i < 3; ++i)
        REQUIRE(client.send({ .url = server.url("/hello") }).wait());

    CHECK(client.idleConnections() == 1);
    CHECK(server.listener->connectionCount() == 1);
}

TEST_CASE("A connection the server closed is replaced transparently", "[client][keepalive]")
{
    HttpClient client;
    unsigned short port;

    {
        Server first;
        port = first.port;
        REQUIRE(client.send({ .url = first.url("/hello") }).wait());
        CHECK(client.idleConnections() == 1);
        // the server stops: the kept connection is closed on its side
    }

    Server second(false, port);
    auto result = client.send({ .url = second.url("/hello") }).wait();
    REQUIRE(result);
    CHECK(result->body() == "hello");
}

TEST_CASE("Request timeout", "[client][timeout]")
{
    Server server;
    HttpClient client;

    auto t0 = std::chrono::steady_clock::now();
    auto result = client.send({ .url = server.url("/slow"), .timeout = 100ms }).wait();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE_FALSE(result);
    CHECK(result.error() == client_error::timeout);
    CHECK(elapsed < 400ms);
}

TEST_CASE("Cancel", "[client]")
{
    Server server;
    HttpClient client;

    std::promise<boost::system::error_code> completed;
    auto call = client.send({ .url = server.url("/slow") }, {
        .onComplete = [&](HttpClient::Result r) { completed.set_value(r ? boost::system::error_code{} : r.error()); },
    });

    std::this_thread::sleep_for(50ms);
    call.cancel();

    auto future = completed.get_future();
    REQUIRE(future.wait_for(1s) == std::future_status::ready);
    CHECK(future.get() == client_error::cancelled);
}

TEST_CASE("Responses over max_response_size fail", "[client]")
{
    Server server;
    HttpClient::Options options;
    options.max_response_size = 1024;
    HttpClient client(options);

    auto small = client.send({ .url = server.url("/big?size=1000") }).wait();
    REQUIRE(small);

    auto big = client.send({ .url = server.url("/big?size=10000") }).wait();
    REQUIRE_FALSE(big);
    CHECK(big.error() == client_error::response_too_large);
}

TEST_CASE("Bad URLs and unreachable servers are errors", "[client][errors]")
{
    HttpClient client;

    auto bad = client.send({ .url = "not a url" }).wait();
    REQUIRE_FALSE(bad);
    CHECK(bad.error() == client_error::invalid_url);

    auto ftp = client.send({ .url = "ftp://example.com/file" }).wait();
    REQUIRE_FALSE(ftp);
    CHECK(ftp.error() == client_error::unsupported_scheme);

    // Port 1 on localhost: nothing listens there
    auto refused = client.send({ .url = "http://127.0.0.1:1/" }).wait();
    REQUIRE_FALSE(refused);
    CHECK(refused.error() == boost::asio::error::connection_refused);
}

TEST_CASE("HTTPS", "[client][tls]")
{
    Server server(true);

    SECTION("trusted CA and matching host name")
    {
        HttpClient client(trustingTestCa());
        auto result = client.send({ .url = server.url("/hello", "https", "localhost") }).wait();
        REQUIRE(result);
        CHECK(result->body() == "hello");

        // Kept alive over TLS too
        REQUIRE(client.send({ .url = server.url("/hello", "https", "localhost") }).wait());
        CHECK(client.idleConnections() == 1);
    }

    SECTION("unknown CA is refused")
    {
        HttpClient client;      // only the bundled Mozilla CAs
        auto result = client.send({ .url = server.url("/hello", "https", "localhost") }).wait();
        REQUIRE_FALSE(result);
        CHECK(result.error().category() == boost::asio::error::get_ssl_category());
    }

    SECTION("host name not in the certificate is refused")
    {
        HttpClient client(trustingTestCa());
        auto result = client.send({ .url = server.url("/hello", "https", "127.0.0.1") }).wait();      // certificate is for localhost
        REQUIRE_FALSE(result);
        CHECK(result.error().category() == boost::asio::error::get_ssl_category());
    }

    SECTION("verify_peer off accepts anything (development only)")
    {
        HttpClient::Options options;
        options.verify_peer = false;
        HttpClient client(options);
        REQUIRE(client.send({ .url = server.url("/hello", "https", "127.0.0.1") }).wait());
    }

    SECTION("upload over TLS")
    {
        HttpClient client(trustingTestCa());
        const std::string body(512 * 1024, 't');
        auto result = client.send({ .method = http::verb::put, .url = server.url("/echo", "https", "localhost"), .body = body }).wait();
        REQUIRE(result);
        CHECK(result->body() == body);
    }
}

TEST_CASE("Trusted CAs from the file system", "[client][tls][ca]")
{
    Server server(true);
    TempDir dir;
    auto url = server.url("/hello", "https", "localhost");

    HttpClient::Options options;
    options.use_bundled_cas = false;

    SECTION("a PEM file")
    {
        options.extra_ca_files = { dir.write("company-ca.pem", test_certificates::ca) };
        HttpClient client(options);
        CHECK(client.send({ .url = url }).wait().has_value());
    }

    SECTION("a hashed directory, as /etc/ssl/certs")
    {
        std::filesystem::create_directories(dir.path / "certs");
        dir.write("certs/" + hashedName(test_certificates::ca), test_certificates::ca);
        options.extra_ca_dirs = { (dir.path / "certs").string() };

        HttpClient client(options);
        CHECK(client.send({ .url = url }).wait().has_value());
    }

    SECTION("the system store, pointed to with SSL_CERT_FILE")
    {
        auto file = dir.write("system-bundle.pem", test_certificates::ca);
        #ifdef _WIN32
            _putenv_s("SSL_CERT_FILE", file.c_str());
        #else
            setenv("SSL_CERT_FILE", file.c_str(), 1);
        #endif

        options.use_system_cas = true;
        HttpClient client(options);

        #ifdef _WIN32
            _putenv_s("SSL_CERT_FILE", "");
        #else
            unsetenv("SSL_CERT_FILE");
        #endif

        CHECK(client.send({ .url = url }).wait().has_value());
    }

    SECTION("files that don't exist fail at construction")
    {
        options.extra_ca_files = { (dir.path / "missing.pem").string() };
        CHECK_THROWS_AS(HttpClient(options), boost::system::system_error);

        options.extra_ca_files.clear();
        options.extra_ca_dirs = { (dir.path / "missing").string() };
        CHECK_THROWS_AS(HttpClient(options), boost::system::system_error);
    }

    SECTION("no CA source at all fails at construction, unless verification is off")
    {
        CHECK_THROWS_AS(HttpClient(options), std::invalid_argument);

        options.verify_peer = false;
        CHECK_NOTHROW(HttpClient(options));
    }
}

TEST_CASE("The bundled Mozilla CA store loads", "[client][tls]")
{
    // Parsing ~120 certificates must not throw
    CHECK_NOTHROW(HttpClient());
}

TEST_CASE("Callbacks can run on a given executor", "[client]")
{
    Server server;
    HttpClient client;

    boost::asio::io_context app;
    auto guard = boost::asio::make_work_guard(app);
    std::thread::id app_thread;
    std::thread runner([&] { app_thread = std::this_thread::get_id(); app.run(); });

    std::promise<std::thread::id> ran_on;
    client.send({ .url = server.url("/hello") }, {
        .onComplete = [&](HttpClient::Result) { ran_on.set_value(std::this_thread::get_id()); },
        .executor = app.get_executor(),
    });

    auto future = ran_on.get_future();
    REQUIRE(future.wait_for(2s) == std::future_status::ready);
    auto id = future.get();

    guard.reset();
    runner.join();
    CHECK(id == app_thread);
}

TEST_CASE("wait() from a client callback returns an error instead of deadlocking", "[client]")
{
    Server server;
    HttpClient client;

    std::promise<boost::system::error_code> inner;
    client.send({ .url = server.url("/hello") }, {
        .onComplete = [&](HttpClient::Result) {
            auto slow = client.send({ .url = server.url("/slow") });
            auto r = slow.wait();
            inner.set_value(r ? boost::system::error_code{} : r.error());
        },
    });

    auto future = inner.get_future();
    REQUIRE(future.wait_for(2s) == std::future_status::ready);
    CHECK(future.get() == client_error::wait_on_client_thread);
}

TEST_CASE("Destroying the client completes the requests in flight", "[client]")
{
    Server server;
    std::promise<boost::system::error_code> completed;
    std::optional<HttpClient::Call> call;

    {
        HttpClient client;
        call = client.send({ .url = server.url("/slow") }, {
            .onComplete = [&](HttpClient::Result r) { completed.set_value(r ? boost::system::error_code{} : r.error()); },
        });
        std::this_thread::sleep_for(50ms);
    }

    auto future = completed.get_future();
    REQUIRE(future.wait_for(1s) == std::future_status::ready);
    CHECK(future.get() == client_error::closed);

    auto r = call->wait();
    REQUIRE_FALSE(r);
    CHECK(r.error() == client_error::closed);
}

TEST_CASE("Many concurrent requests", "[client][threads]")
{
    Server server;
    HttpClient::Options options;
    options.threads = 4;
    HttpClient client(options);

    std::vector<HttpClient::Call> calls;
    for (int i = 0; i < 100; ++i)
        calls.push_back(client.send({ .url = server.url("/big?size=" + std::to_string(1000 + i)) }));

    int ok = 0;
    for (int i = 0; i < 100; ++i)
    {
        auto r = calls[i].wait();
        if (r && r->body().size() == static_cast<std::size_t>(1000 + i))
            ++ok;
    }
    CHECK(ok == 100);
    CHECK(client.idleConnections() <= options.max_idle_per_host);
}

// Needs the internet, so hidden: ./test_http_client "[network]"
TEST_CASE("Real HTTPS sites verify against the bundled Mozilla CAs", "[.][network]")
{
    HttpClient client;

    for (const char* url : { "https://curl.se/ca/cacert.pem.sha256", "https://www.google.com/", "https://github.com/", "https://www.cloudflare.com/" })
    {
        auto result = client.send({ .method = http::verb::head, .url = url, .timeout = 15s }).wait();
        INFO(url << ": " << (result ? std::string("HTTP ") + std::to_string(result->status()) : result.error().message()));
        CHECK(result.has_value());
    }

    // The operating system's store instead of the bundle (on macOS: /etc/ssl/cert.pem)
    HttpClient::Options system;
    system.use_bundled_cas = false;
    system.use_system_cas = true;
    HttpClient system_client(system);
    auto via_system = system_client.send({ .method = http::verb::head, .url = "https://www.google.com/", .timeout = 15s }).wait();
    INFO("system store: " << (via_system ? std::string("HTTP ") + std::to_string(via_system->status()) : via_system.error().message()));
    CHECK(via_system.has_value());

    // And a bad certificate is still refused (badssl.com hosts deliberately broken ones)
    auto expired = client.send({ .url = "https://expired.badssl.com/", .timeout = 15s }).wait();
    INFO("expired.badssl.com: " << (expired ? "accepted!" : expired.error().message()));
    CHECK_FALSE(expired.has_value());
}
