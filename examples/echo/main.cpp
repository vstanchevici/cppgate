#include <cppgate/Listener.h>

#include <cstdlib>
#include <memory>
#include <string>

namespace http = boost::beast::http;

/*
    HTTP + WebSocket echo server.

        curl http://127.0.0.1:8080/hello
        ws://127.0.0.1:8080/ws/echo/{room}?token=secret
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
    listener->Run<gtvr::Protocol::HTTP | gtvr::Protocol::WS>();

    slog_info("listening on 127.0.0.1:{}", port);

    ioc.run();

    return 0;
}
