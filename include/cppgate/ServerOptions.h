#pragma once

#include <chrono>
#include <cstddef>

namespace gtvr {

    // Limits and timeouts of a Listener and its sessions. The defaults are safe for small APIs,
    // raise http_body_limit for uploads.
    struct ServerOptions
    {
        // --- HTTP ---

        // Request line + headers. Over it: 414 URI Too Long (request line) or 431 Request Header Fields Too Large.
        std::size_t                 http_header_limit           = 8 * 1024;

        // Request body. Over it: 413 Content Too Large.
        std::size_t                 http_body_limit             = 10000;

        // Time to read one request (and for the TLS detection / handshake / shutdown)
        std::chrono::seconds        http_timeout                { 30 };

        // After an error response (400/413/414/431) the server stops reading requests but keeps
        // discarding what the client still sends for this long, so the client gets to read the
        // response instead of a connection reset.
        std::chrono::milliseconds   http_linger_timeout         { 2000 };

        // --- WebSocket ---

        std::size_t                 websocket_message_limit     = 16 * 1024 * 1024;

        // Opening and closing handshakes
        std::chrono::seconds        websocket_handshake_timeout { 30 };

        // Close the connection after this long without data (pings count when keep_alive_pings is on)
        std::chrono::seconds        websocket_idle_timeout      { 300 };

        bool                        websocket_keep_alive_pings  = true;

        // Messages waiting to be sent to one client. A client that can't keep up past this is
        // disconnected with 1008 (policy violation) instead of growing memory without limit.
        std::size_t                 websocket_send_queue_limit  = 32;
    };

}//namespace gtvr
