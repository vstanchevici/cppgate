#include <cppgate/Listener.h>

#include <boost/asio/ssl.hpp>

#include <cstdlib>
#include <memory>
#include <string>

namespace http = boost::beast::http;

/*
    HTTP + WebSocket echo server.

        echo_server [port] [cert.pem key.pem [tls-only]]

        curl http://127.0.0.1:8080/hello
        ws://127.0.0.1:8080/ws/echo/{room}?token=secret

    With a certificate, HTTP and HTTPS (WS and WSS) on the same port, or HTTPS only with "tls-only".
    Self-signed certificate for testing:
        openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 365 -subj "/CN=localhost"
*/
int main(int argc, char* argv[])
{
    gtvr::Logger::Init();

    unsigned short port = argc > 1 ? static_cast<unsigned short>(std::atoi(argv[1])) : 8080;

    boost::asio::io_context ioc;

    auto router = std::make_shared<gtvr::router::Router>();
    // Behind nginx on the same machine: believe its X-Forwarded-For
    router->setTrustedProxies({ "127.0.0.1" });

    router->add({ http::verb::get }, "/hello", [](gtvr::router::HttpRequest&, gtvr::router::HttpResponse& res) {
        res.send("hello from cppgate", "text/plain");
    });

    // Runs before the handshake: refuse the upgrade without the right token
    gtvr::router::Handler auth = gtvr::router::WebSocketHandler([](std::shared_ptr<gtvr::WebSocketContext> ws) {
        gtvr::router::HttpRequest request(ws->request());

        if (request.urlQueries().get("token") != "secret")
        {
            ws->reject(http::status::unauthorized, "bad token");
            return false;
        }

        ws->setContext(std::make_shared<std::string>(ws->urlParam("room")));
        return true;
    });

    // onOpen: set the callbacks
    router->add("/ws/echo/{room}", { auth }, [](std::shared_ptr<gtvr::WebSocketContext> ws) {
        slog_info("open {} room={}", ws->ip(), *ws->getContext<std::string>());

        ws->onMessage([](gtvr::WebSocketContext& ws, const gtvr::WebSocketMessage& msg) {
            if (msg.text() == "bye")
                ws.close();
            else if (msg.isText())
                ws.sendText("[" + *ws.getContext<std::string>() + "] " + std::string(msg.text()));
            else
                ws.sendBinary(std::vector<uint8_t>(msg.bytes().begin(), msg.bytes().end()));
        });

        ws->onClose([](gtvr::WebSocketContext& ws, gtvr::WebSocketContext::CloseCode code) {
            slog_info("close {} code={}", ws.ip(), static_cast<int>(code));
        });

        ws->onError([](gtvr::WebSocketContext& ws, boost::beast::error_code ec) {
            slog_warn("error {}: {}", ws.ip(), ec.message());
        });

        return true;
    });

    auto listener = std::make_shared<gtvr::Listener>(ioc, boost::asio::ip::tcp::endpoint{ boost::asio::ip::make_address("127.0.0.1"), port });
    listener->SetRouter(router);

    gtvr::ServerOptions options;
    options.http_body_limit = 64 * 1024;        // accept request bodies up to 64 KB
    listener->SetOptions(options);
    // Must outlive the listener
    boost::asio::ssl::context ssl_ctx{ boost::asio::ssl::context::tls_server };

    if (argc > 3)
    {
        ssl_ctx.set_options(boost::asio::ssl::context::default_workarounds
                          | boost::asio::ssl::context::no_sslv2
                          | boost::asio::ssl::context::no_sslv3
                          | boost::asio::ssl::context::no_tlsv1
                          | boost::asio::ssl::context::no_tlsv1_1);
        ssl_ctx.use_certificate_chain_file(argv[2]);
        ssl_ctx.use_private_key_file(argv[3], boost::asio::ssl::context::pem);
        listener->SetSSLContext(&ssl_ctx);
    }

    if (argc > 4 && std::string(argv[4]) == "tls-only")
    {
        listener->Run<gtvr::Protocol::HTTPS | gtvr::Protocol::WSS>();
        slog_info("listening on 127.0.0.1:{} (HTTPS/WSS only)", port);
    }
    else if (argc > 3)
    {
        listener->Run<gtvr::Protocol::HTTP | gtvr::Protocol::HTTPS | gtvr::Protocol::WS | gtvr::Protocol::WSS>();
        slog_info("listening on 127.0.0.1:{} (HTTP/WS and HTTPS/WSS)", port);
    }
    else
    {
        listener->Run<gtvr::Protocol::HTTP | gtvr::Protocol::WS>();
        slog_info("listening on 127.0.0.1:{} (HTTP/WS)", port);
    }

    ioc.run();

    return 0;
}
