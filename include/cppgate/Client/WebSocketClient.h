#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/system/error_code.hpp>
#include <cppgate/Client/ClientTlsOptions.h>
#include <cppgate/Client/HttpClient.h>          // client_error
#include <cppgate/WebSocketContext.h>           // WebSocketMessage, close codes
#include <cppgate/cppgate.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gtvr {

    namespace detail {
        struct WsClientImpl;
        struct WsConnection;
    }

    /*
        Asynchronous WebSocket client, ws:// and wss://, with automatic reconnect.

            gtvr::WebSocketClient client;      // threads + TLS, shared by its connections

            auto ws = client.connect(
                { .url = "wss://stream.example.com/v1", .reconnect = { .enabled = true } },
                {
                    .onOpen    = [](auto& ws) { ws.sendText(R"({"subscribe":"prices"})"); },   // after every (re)connect
                    .onMessage = [](auto& ws, const gtvr::WebSocketMessage& msg) { msg.text(); },
                    .onClose   = [](auto& ws, gtvr::WebSocketClient::CloseCode code) { },
                    .onError   = [](auto& ws, boost::system::error_code ec) { },
                });

            ws->sendText("hello");      // false if not connected or the send queue is full
            ws->close();                // no reconnect after this

        Events:
        - onOpen after every successful connect; onClose exactly once for every opened connection.
        - A failed attempt (DNS, refused, TLS, timeout) calls onError, then onReconnecting if a retry follows.
        - Reconnect follows any close we didn't ask for, and failed attempts. Not after close(), and not
          when the server answers the upgrade with an HTTP status (client_error::upgrade_rejected,
          see upgradeStatus()): retrying wouldn't change that answer.
        - When Reconnect::max_attempts is used up: onError(client_error::gave_up), then nothing.

        Callbacks of one connection run one at a time, on the client's threads unless Callbacks::executor
        is set. They receive the connection as a parameter: don't capture its shared_ptr in them.
        Dropping the last shared_ptr returned by connect() closes the connection.
    */
    class CPPGATE_API WebSocketClient
    {
        public:
            using CloseCode = WebSocketContext::CloseCode;

            // TLS / CA settings come from ClientTlsOptions (verify_peer, use_bundled_cas, use_system_cas, extra_ca_*)
            struct Options : ClientTlsOptions
            {
                std::size_t                 threads             = 1;

                // DNS + TCP + TLS + WebSocket upgrade of one attempt
                std::chrono::milliseconds   connect_timeout     { 10000 };

                // Largest message received; a bigger one closes the connection (1009)
                std::size_t                 max_message_size    = 16 * 1024 * 1024;

                // Ping when nothing was received for half of it, consider the connection dead after it
                std::chrono::milliseconds   idle_timeout        { 30000 };
                bool                        keep_alive_pings    = true;

                // How long close() waits for the server to answer the close
                std::chrono::milliseconds   close_timeout       { 5000 };

                // Messages waiting to be written; over it send*() returns false and drops the message
                std::size_t                 send_queue_limit    = 32;

                // permessage-deflate, if the server agrees
                bool                        compression         = false;

                std::string                 user_agent          = "cppgate";
            };

            struct Reconnect
            {
                bool                        enabled             = false;

                // Delay before the first retry; doubles after each failed one, up to max_delay, +-20% jitter.
                // Reset after a successful connect.
                std::chrono::milliseconds   initial_delay       { 500 };
                std::chrono::milliseconds   max_delay           { 30000 };

                // Retries in a row without success; 0 = no limit
                unsigned                    max_attempts        = 0;
            };

            struct Request
            {
                std::string                                         url;
                std::vector<std::pair<std::string, std::string>>    headers;        // Authorization, Origin...
                std::vector<std::string>                            subprotocols;   // Sec-WebSocket-Protocol
                Reconnect                                           reconnect;
            };

            class CPPGATE_API Connection
            {
                public:
                    virtual ~Connection() = default;

                    // From any thread. False (and the message is dropped) when not connected or the send
                    // queue is full. True means queued: it is lost if the connection drops before it's written.
                    virtual bool sendText(std::string data) = 0;
                    virtual bool sendText(std::shared_ptr<const std::string> data) = 0;
                    virtual bool sendBinary(std::vector<std::uint8_t> data) = 0;

                    // From any thread. Closes (or stops connecting) for good: no reconnect after it.
                    virtual void close(CloseCode code = CloseCode::normal) = 0;

                    virtual bool isConnected() const = 0;

                    // The subprotocol the server chose, empty if none
                    virtual std::string protocol() const = 0;

                    // HTTP status of the last rejected upgrade (client_error::upgrade_rejected), 0 otherwise
                    virtual unsigned upgradeStatus() const = 0;

                    // Queued, not written yet
                    virtual std::size_t pendingMessages() const = 0;
            };

            struct Callbacks
            {
                std::function<void(Connection&)>                                                    onOpen;
                std::function<void(Connection&, const WebSocketMessage&)>                           onMessage;
                std::function<void(Connection&, CloseCode)>                                         onClose;
                std::function<void(Connection&, boost::system::error_code)>                         onError;
                std::function<void(Connection&, unsigned attempt, std::chrono::milliseconds delay)> onReconnecting;

                // Where the callbacks run (e.g. a strand). Default: the client's threads.
                boost::asio::any_io_executor                                                        executor;
            };

        private:
            std::shared_ptr<detail::WsClientImpl> impl_;

        public:
            WebSocketClient();

            // Throws like HttpClient for CA sources that can't be loaded or are missing
            explicit WebSocketClient(Options options);

            // Closes every connection (1001 going away, waiting up to close_timeout), stops the threads.
            // Don't destroy the client from one of its own callbacks.
            ~WebSocketClient();

            WebSocketClient(const WebSocketClient&) = delete;
            WebSocketClient& operator=(const WebSocketClient&) = delete;

            // Starts connecting; events come through the callbacks. An invalid URL is reported with onError.
            std::shared_ptr<Connection> connect(Request request, Callbacks callbacks = {});
    };

}//namespace gtvr
