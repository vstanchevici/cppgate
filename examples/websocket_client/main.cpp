#include <cppgate/Client/WebSocketClient.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>

#include <atomic>
#include <iostream>
#include <thread>

using gtvr::WebSocketClient;

/*
    WebSocketClient example.

        websocket_client_example [url]        default: wss://echo.websocket.org/

    Connects with automatic reconnect, sends a message every second, prints what comes back,
    and closes cleanly on Ctrl+C.
*/
int main(int argc, char* argv[])
{
    std::string url = argc > 1 ? argv[1] : "wss://echo.websocket.org/";

    // ---- One client for the program: its threads and TLS settings serve every connection
    WebSocketClient::Options options;
    options.idle_timeout = std::chrono::seconds(20);      // ping when quiet, detect dead connections
    // options.use_bundled_cas = false; options.use_system_cas = true;     // or the OS CA store

    WebSocketClient client(options);

    // ---- Connect: events come through the callbacks, on the client's threads
    auto ws = client.connect(
        {
            .url       = url,
            .headers   = { { "X-Client", "cppgate example" } },
            .reconnect = { .enabled = true, .initial_delay = std::chrono::seconds(1), .max_delay = std::chrono::seconds(30) },
        },
        {
            .onOpen = [](WebSocketClient::Connection& ws) {
                // Called after every (re)connect: subscribe / authenticate here
                std::cout << "connected\n";
                ws.sendText("hello from cppgate");
            },
            .onMessage = [](WebSocketClient::Connection&, const gtvr::WebSocketMessage& msg) {
                if (msg.isText())
                    std::cout << "received: " << msg.text() << "\n";
                else
                    std::cout << "received " << msg.size() << " binary bytes\n";
            },
            .onClose = [](WebSocketClient::Connection&, WebSocketClient::CloseCode code) {
                std::cout << "closed: " << static_cast<int>(code) << "\n";
            },
            .onError = [](WebSocketClient::Connection& ws, boost::system::error_code ec) {
                if (ec == gtvr::client_error::upgrade_rejected)
                    std::cout << "rejected by the server: HTTP " << ws.upgradeStatus() << "\n";
                else
                    std::cout << "error: " << ec.message() << "\n";
            },
            .onReconnecting = [](WebSocketClient::Connection&, unsigned attempt, std::chrono::milliseconds delay) {
                std::cout << "reconnecting (attempt " << attempt << ") in " << delay.count() << " ms\n";
            },
        });

    // ---- Send from another thread: sendText() is thread-safe, false when not connected / queue full
    std::atomic<bool> running{ true };
    std::thread sender([&] {
        for (int i = 1; running; ++i)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!ws->sendText("message " + std::to_string(i)))
                std::cout << "(not sent: not connected)\n";
        }
    });

    // ---- Ctrl+C: close cleanly, then let the client go
    boost::asio::io_context signals_ioc;
    boost::asio::signal_set signals(signals_ioc, SIGINT, SIGTERM);
    signals.async_wait([&](const boost::system::error_code&, int) {
        running = false;
        ws->close();            // onClose follows; no reconnect after close()
    });
    signals_ioc.run();

    sender.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));     // let onClose print
}
