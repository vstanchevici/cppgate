#include <cppgate/Client/HttpClient.h>
#include <cppgate/Listener.h>

#include <boost/asio/signal_set.hpp>

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>

namespace http = boost::beast::http;
using gtvr::HttpClient;

/*
    HttpClient example.

        http_client_example [file-to-upload]

    1. creates one client trusting the operating system's CA store
    2. GETs a JSON API and waits for the result
    3. uploads a file asynchronously with progress (if one is given)
    4. serves /weather/{city} on 127.0.0.1:8080, calling another API from the handler
*/
int main(int argc, char* argv[])
{
    gtvr::Logger::Init();

    // ---- 1. One client for the whole program: it keeps connections alive and reuses them
    HttpClient::Options options;
    options.use_bundled_cas = false;        // trust the OS store instead of the bundle:
    options.use_system_cas  = true;         //   Ubuntu: /etc/ssl/certs/ca-certificates.crt
    options.request_timeout = std::chrono::seconds(15);
    options.threads = 2;

    HttpClient client(options);             // throws if no CA store is found

    // ---- 2. GET and wait for the result (fine from main or a synchronous handler)
    auto result = client.send({
        .url     = "https://api.github.com/repos/vstanchevici/cppgate",
        .headers = { { "Accept", "application/json" } },
    }).wait();

    if (!result)
        std::cerr << "request failed: " << result.error().message() << "\n";   // DNS, TLS, timeout...
    else if (result->status() != 200)
        std::cerr << "HTTP " << result->status() << ": " << result->body() << "\n";
    else
        std::cout << "got " << result->body().size() << " bytes of JSON\n";

    // ---- 3. Upload a file with progress, fully asynchronous
    std::optional<HttpClient::Call> upload;

    if (argc > 1)
    {
        std::ifstream file(argv[1], std::ios::binary);
        if (!file)
        {
            std::cerr << "can't read " << argv[1] << "\n";
            return 1;
        }

        std::stringstream content;
        content << file.rdbuf();

        upload = client.send(
            {
                .method  = http::verb::post,
                .url     = "https://upload.example.com/files",       // replace with your upload endpoint
                .headers = { { "Content-Type", "application/octet-stream" }, { "Authorization", "Bearer TOKEN" } },
                .body    = content.str(),
                .timeout = std::chrono::minutes(5),         // overrides request_timeout for this one
            },
            {
                .onProgress = [](const HttpClient::Progress& p) {
                    const char* what = p.direction == HttpClient::Progress::Direction::upload ? "upload" : "download";
                    if (p.total)
                        std::cout << what << " " << (100 * p.transferred / std::max<std::size_t>(*p.total, 1)) << "%\n";
                    else
                        std::cout << what << " " << p.transferred << " bytes\n";
                },
                .onComplete = [](HttpClient::Result r) {        // exactly once
                    if (!r)
                    {
                        if (r.error() == gtvr::client_error::timeout)
                            std::cerr << "upload timed out\n";
                        else
                            std::cerr << "upload failed: " << r.error().message() << "\n";
                        return;
                    }
                    std::cout << "uploaded: HTTP " << r->status() << "\n";
                },
            });

        // upload->cancel();    // from any thread, if the user gives up
    }

    // ---- 4. From a cppgate handler: call another API and answer with its result
    boost::asio::io_context ioc;
    auto router = std::make_shared<gtvr::router::Router>();

    router->add({ http::verb::get }, "/weather/{city}", [&client](gtvr::router::HttpRequest& req, gtvr::router::HttpResponse& res) {
        auto r = client.send({
            .url     = "https://api.weather.example/v1/now?city=" + std::string(req.urlParam("city")),   // replace with a real API
            .timeout = std::chrono::seconds(3),             // keep it short: the handler waits
        }).wait();

        if (!r)
            return res.send(http::status::bad_gateway, "weather service unavailable: " + r.error().message());

        res.send(static_cast<http::status>(r->status()), r->body(), "application/json");
    });

    auto listener = std::make_shared<gtvr::Listener>(ioc, boost::asio::ip::tcp::endpoint{ boost::asio::ip::make_address("127.0.0.1"), 8080 });
    listener->setRouter(router);
    listener->run<gtvr::Protocol::HTTP>();

    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([listener](const boost::system::error_code& ec, int) { if (!ec) listener->stop(); });

    if (upload)
        upload->wait();     // let the upload finish before serving (only so the output stays readable)

    std::cout << "serving http://127.0.0.1:8080/weather/{city}, Ctrl+C to stop\n";
    ioc.run();
}
