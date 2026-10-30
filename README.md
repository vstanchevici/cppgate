# cppgate

A C++20 HTTP and WebSocket server library built on Boost.Beast and Boost.Asio: an Express-style router, WebSocket routes with middleware, TLS, and production basics (limits, client IP behind proxies, graceful shutdown).

> **Status: early (v0.1.0).** The API may still change. Developed and tested on macOS (Apple Clang 14, Xcode 14). Linux and Windows have build scripts but have not been tested yet.

## Features

- **Router**: paths with parameters (`/users/{id}`, `/items/{id:\d+}`), method sets, groups with prefixes, global and per-route middleware, custom 404/405 handlers, 500 on exceptions. Patterns use RE2, so a route can't trigger catastrophic regex backtracking.
- **WebSocket routes** with the same router and middleware: the upgrade can be refused with a real HTTP status before the handshake, and callbacks (`onMessage`, `onClose`, `onError`) are set per connection.
- **HTTP and HTTPS on one port** (TLS detected per connection), or HTTPS only. WSS works the same way.
- **Limits and errors**: header/body limits with proper `400`/`413`/`414`/`431` responses, `Expect: 100-continue`, timeouts, slow WebSocket clients disconnected instead of buffering without limit.
- **CORS** middleware, preflight included.
- **Client IP behind reverse proxies** (`X-Forwarded-For`, `X-Real-IP`), only trusted from proxies you declare.
- **Graceful shutdown**: stop accepting, finish requests in progress, close WebSockets with 1001.
- **HTTP client**: asynchronous HTTP/HTTPS with upload/download progress, keep-alive, timeouts and the Mozilla CA store built in.
- **WebSocket client**: ws/wss with automatic reconnect, keep-alive pings and the same message API as the server.
- **Connection pool** for databases or any other client: thread-safe, acquire with timeout, validation of idle connections.

## Requirements

- C++20 compiler, CMake 3.20+, [Conan 2](https://conan.io)
- Dependencies (installed by Conan): Boost 1.86, OpenSSL 3, RE2, spdlog. Catch2 for the tests.

## Build

```sh
# 1. Dependencies (macOS; Linux: BuildLinux.sh, Windows: BuildVS2022.bat)
cd conan && ./BuildMacOS.sh Debug && cd ..

# 2. Configure and build, with tests and examples
cmake -S . -G Xcode -B build/xcode \
  -DCMAKE_TOOLCHAIN_FILE=build/xcode/generators/conan_toolchain.cmake \
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON
cmake --build build/xcode --config Debug

# 3. Run the tests and the example
ctest --test-dir build/xcode -C Debug
./build/xcode/Debug/echo_server 8080
```

CMake options: `BUILD_TESTING`, `BUILD_EXAMPLES` (both `OFF` by default), `BUILD_SHARED_LIBS` (static by default).

## Quick start

```cpp
#include <cppgate/Listener.h>

namespace http = boost::beast::http;
using gtvr::router::HttpRequest;
using gtvr::router::HttpResponse;

int main()
{
    gtvr::Logger::Init();       // required before anything logs

    boost::asio::io_context ioc;

    auto router = std::make_shared<gtvr::router::Router>();

    router->add({ http::verb::get }, "/hello", [](HttpRequest&, HttpResponse& res) {
        res.send("hello", "text/plain");
    });

    router->add({ http::verb::get }, "/users/{id}", [](HttpRequest& req, HttpResponse& res) {
        res.sendJSON(R"({"id": ")" + std::string(req.urlParam("id")) + R"("})");
    });

    auto listener = std::make_shared<gtvr::Listener>(ioc, boost::asio::ip::tcp::endpoint{ boost::asio::ip::make_address("0.0.0.0"), 8080 });
    listener->setRouter(router);
    listener->run<gtvr::Protocol::HTTP>();

    ioc.run();
}
```

The constructor throws `boost::system::system_error` if the address can't be used, for example when the port is already taken. [`examples/echo`](examples/echo/main.cpp) is a complete server with WebSockets, TLS, options and graceful shutdown.

## Routing

```cpp
router->add({ http::verb::get, http::verb::head }, "/files/{name}", handler);    // several methods
router->add({ http::verb::get }, R"(/items/{id:\d+})", handler);                 // custom pattern: /items/42, not /items/abc
```

In a handler:

| | |
|---|---|
| `req.urlParam("id")` | path parameter (a view, valid during the handler) |
| `req.urlQueries().get("q")` | query string value |
| `req.formQueries().get("user")` | `application/x-www-form-urlencoded` body |
| `req.headers().get(http::field::authorization)` | header |
| `req.body()`, `req.method()`, `req.target()` | the rest of the request |
| `req.getIP()` | client IP (see [Client IP](#client-ip-behind-a-proxy)) |
| `res.send(status, body, content_type)`, `res.sendJSON(body)`, `res.sendNoContent()` | responses |

- The first registered route that matches wins. A known path with the wrong method gets 405, an unknown path 404. Replace them with `setNotFoundHandler()` / `setMethodNotAllowedHandler()`.
- A handler that throws, or returns without a response, produces 500.

### Middleware and groups

Middleware has the same signature as a handler. A **pre** middleware that sends a response stops the chain; a **post** middleware runs only if the handler didn't respond.

```cpp
gtvr::router::Handler auth = gtvr::router::HttpHandler([](HttpRequest& req, HttpResponse& res) {
    if (req.headers().get(http::field::authorization) != "Bearer secret")
        res.sendUnauthorized("login first");
});

router->group("/api/v1", { auth }, {}, [](gtvr::router::Router& r) {
    r.add({ http::verb::get }, "/me", [](HttpRequest& req, HttpResponse& res) { /* ... */ });
});
```

Use `req.setContext(std::make_shared<User>(...))` in a middleware and `req.getContext<User>()` in the handler to pass data along. A group can mix HTTP and WebSocket middleware: each route only runs the middleware of its own kind.

`router->use(middleware)` adds a middleware for **every** HTTP request, run before route matching, so it also sees requests that end in 404 or 405 (logging, request IDs, rate limiting, CORS). Headers it sets stay on the final response.

### CORS

```cpp
#include <cppgate/Middleware/Cors.h>

gtvr::middleware::CorsOptions cors;
cors.allowed_origins = { "https://app.example.com", "https://*.example.com", "http://localhost:3000" };
cors.allow_credentials = true;      // cookies / Authorization
router->use(gtvr::middleware::cors(cors));
```

Register it with `use()`: browsers send a preflight `OPTIONS` request first, and it must be answered whatever methods the route has. Preflights from allowed origins get `204` (handlers don't run), from other origins `403`. Other requests from an allowed origin get the CORS headers on their response, including errors, so the page can read them.

| Option | Default | |
|---|---|---|
| `allowed_origins` | `{ "*" }` | exact origins, `https://*.example.com` for subdomains, or `*` |
| `allowed_methods` | GET, HEAD, POST, PUT, PATCH, DELETE | |
| `allowed_headers` | empty | request headers allowed; empty allows what the preflight asks for |
| `exposed_headers` | empty | response headers JavaScript may read |
| `allow_credentials` | `false` | with `*`, the request's origin is echoed (browsers refuse `*` with credentials) |
| `max_age` | 600 s | how long browsers cache a preflight |

CORS is enforced by browsers only. It is not access control: protect your routes anyway.

## WebSockets

A WebSocket route works in two steps:

1. **Before the handshake**, the pre-middleware decides. Returning `false` refuses the upgrade with an HTTP response: 403 by default, or whatever you set with `reject()`.
2. **After the handshake**, the handler runs. It is the `onOpen` event: set the callbacks there, or call `close()`.

```cpp
gtvr::router::Handler auth = gtvr::router::WebSocketHandler([](std::shared_ptr<gtvr::WebSocketContext> ws) {
    gtvr::router::HttpRequest request(ws->request());
    if (request.urlQueries().get("token") != "secret")
    {
        ws->reject(http::status::unauthorized, "bad token");
        return false;
    }
    ws->setContext(std::make_shared<std::string>("alice"));     // per-connection data
    return true;
});

router->add("/ws/rooms/{room}", { auth }, [](std::shared_ptr<gtvr::WebSocketContext> ws) {
    ws->onMessage([](gtvr::WebSocketContext& ws, const gtvr::WebSocketMessage& msg) {
        if (msg.isText())
            ws.sendText(*ws.getContext<std::string>() + ": " + std::string(msg.text()));
    });
    ws->onClose([](gtvr::WebSocketContext& ws, gtvr::WebSocketContext::CloseCode code) { /* ... */ });
    ws->onError([](gtvr::WebSocketContext& ws, boost::beast::error_code ec) { /* ... */ });
    return true;
});
```

Guarantees:

- All callbacks of one connection run one at a time, never concurrently.
- Messages are only read after the handler returns, so none arrive before `onMessage` is set.
- `onClose` is called exactly once, however the connection ends (client, server, error).
- `msg.text()` / `msg.bytes()` are only valid during the callback; copy what you keep.
- `sendText()`, `sendBinary()`, `close()` and `pendingMessages()` are safe from any thread.

Callbacks receive the context as a parameter: don't capture the `shared_ptr` in them, the connection owns its callbacks. Keep the `shared_ptr` elsewhere (e.g. a list of connections for broadcasting) and remove it in `onClose`.

A client that reads slower than you send is disconnected with 1008 once `websocket_send_queue_limit` messages are waiting. Check `pendingMessages()` to skip slow clients, for example in a broadcast.

## HTTPS and WSS

```cpp
#include <boost/asio/ssl.hpp>

boost::asio::ssl::context ctx{ boost::asio::ssl::context::tls_server };   // must outlive the listener
ctx.set_options(boost::asio::ssl::context::default_workarounds
              | boost::asio::ssl::context::no_sslv2 | boost::asio::ssl::context::no_sslv3
              | boost::asio::ssl::context::no_tlsv1 | boost::asio::ssl::context::no_tlsv1_1);
ctx.use_certificate_chain_file("fullchain.pem");
ctx.use_private_key_file("privkey.pem", boost::asio::ssl::context::pem);

listener->setSSLContext(&ctx);
listener->run<gtvr::Protocol::HTTP | gtvr::Protocol::HTTPS>();   // both on one port
// listener->run<gtvr::Protocol::HTTPS>();                       // HTTPS only
```

WebSocket upgrades over HTTPS are WSS; nothing else to configure. `run()` throws `std::logic_error` if an HTTPS flag is set without a context. For a development certificate:

```sh
openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 365 -subj "/CN=localhost"
```

Alternatively, terminate TLS in nginx or Caddy and run cppgate with plain HTTP behind it (see the next section for client IPs).

## Client IP behind a proxy

Behind a reverse proxy, the socket peer is the proxy. Declare the proxies you trust:

```cpp
router->setTrustedProxies({ "127.0.0.1", "10.0.0.0/8" });
```

`req.getIP()` / `ws.ip()` then return the client from `X-Forwarded-For` (read right to left, skipping trusted proxies) or `X-Real-IP`. `getPeerIP()` / `peerIP()` always return the socket address. The headers are ignored when the request doesn't come from a trusted proxy, and the list is empty by default, so clients can't fake their IP. The list can be replaced while the server runs (e.g. to refresh CDN ranges).

## Options

```cpp
gtvr::ServerOptions options;
options.http_body_limit = 1024 * 1024;
listener->setOptions(options);      // before run()
```

| Option | Default | |
|---|---|---|
| `http_header_limit` | 8 KB | request line + headers; over it: 414 or 431 |
| `http_body_limit` | 10 000 bytes | over it: 413 |
| `http_timeout` | 30 s | to read one request |
| `http_linger_timeout` | 2 s | after an error response, drain the request so the client can read the response |
| `websocket_message_limit` | 16 MB | largest message received |
| `websocket_handshake_timeout` | 30 s | opening and closing handshakes |
| `websocket_idle_timeout` | 300 s | close idle connections |
| `websocket_keep_alive_pings` | `true` | ping idle clients |
| `websocket_send_queue_limit` | 32 messages | past it, the client is closed with 1008 |

The default body limit is small; raise it if your API receives larger JSON or uploads.

## Graceful shutdown

```cpp
boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
signals.async_wait([listener](const boost::system::error_code& ec, int) {
    if (!ec)
        listener->stop();
});

ioc.run();      // returns once the last connection is closed
```

`stop()` stops accepting, closes idle connections, lets requests in progress finish (with `Connection: close`) and closes WebSockets with 1001. A client that never answers the WebSocket close frame can delay it up to `websocket_handshake_timeout`; call `ioc.stop()` after your own deadline if you need a hard limit.

## HTTP client

`gtvr::HttpClient` (`#include <cppgate/Client/HttpClient.h>`) calls other HTTP/1.1 services, HTTP or HTTPS. It runs on its own threads and reports with callbacks and results, never exceptions.

```cpp
gtvr::HttpClient client;    // one for the whole program: it keeps connections alive

client.send(
    {
        .method  = http::verb::post,
        .url     = "https://api.example.com/v1/files",
        .headers = { { "Authorization", "Bearer ..." }, { "Content-Type", "application/json" } },
        .body    = json,
        .timeout = std::chrono::seconds(10),
    },
    {
        .onProgress = [](const gtvr::HttpClient::Progress& p) {
            // p.direction: upload (the request body) then download, p.transferred of p.total (optional)
        },
        .onComplete = [](gtvr::HttpClient::Result r) {
            if (!r) { r.error().message(); return; }     // DNS, connect, TLS, timeout, cancelled...
            r->status(); r->header("Content-Type"); r->body();
        },
    });

// From a synchronous handler: wait for the result (the client has its own threads, this can't deadlock)
auto r = client.send({ .url = "https://api.example.com/v1/status" }).wait();
```

- `onComplete` is called exactly once. 4xx and 5xx are responses (`r->status()`), not errors. Errors are a `boost::system::error_code`: Asio/OpenSSL codes for network and TLS problems, `gtvr::client_error` for the client's own (`timeout`, `cancelled`, `response_too_large`, `invalid_url`, `unsupported_scheme`, `closed`).
- `onProgress` reports the upload (only with a body) then the download, at most every 64 KB or 100 ms, plus a final event when complete. Upload counts bytes handed to the network.
- `send()` returns a `Call`: `cancel()`, `wait()`, `done()`. Dropping it doesn't cancel the request. `Callbacks::executor` runs the callbacks somewhere else than the client's threads (e.g. a strand).
- Connections are kept alive per host and reused. A request that fails on a connection the server had closed meanwhile is retried once on a new one (idempotent methods, or when the request couldn't be sent).
- Destroying the client completes the requests in flight with `client_error::closed`. Don't destroy it from one of its own callbacks.

| Option | Default | |
|---|---|---|
| `threads` | 1 | threads running the client and its callbacks |
| `connect_timeout` | 10 s | DNS, TCP and TLS of a new connection |
| `request_timeout` | 30 s | whole request; `Request::timeout` overrides it |
| `max_response_size` | 64 MB | over it: `client_error::response_too_large` |
| `max_idle_per_host`, `idle_timeout` | 8, 30 s | kept-alive connections |
| `verify_peer` | `true` | check the certificate and host name; off only for local development |
| `use_bundled_cas` | `true` | trust the Mozilla CA store built into cppgate |
| `use_system_cas` | `false` | trust the operating system's CA store (see below) |
| `extra_ca_files` | empty | PEM files with trusted CAs, e.g. an internal company CA |
| `extra_ca_dirs` | empty | directories in OpenSSL's hashed format (`<hash>.0`, from `openssl rehash`) |
| `extra_ca_pem` | empty | trusted CAs as PEM text |

**Certificates.** The sources can be combined. With `verify_peer` on and no source at all, the constructor throws, so a misconfiguration doesn't show up later as "every HTTPS call fails".

- *Bundled* (default): Mozilla's CA store as published by curl ([curl.se/ca](https://curl.se/ca/)), compiled into cppgate, the same on every OS. Mozilla adds and removes CAs over time: refresh it with `python3 tools/update_ca_bundle.py` (it checks the download's SHA-256), rebuild and commit. The certificate data is under the Mozilla Public License 2.0.
- *System*: `$SSL_CERT_FILE` / `$SSL_CERT_DIR` if set, plus the first well-known bundle found: `/etc/ssl/certs/ca-certificates.crt` (Debian, Ubuntu), `/etc/pki/tls/certs/ca-bundle.crt` (Fedora, RHEL), `/etc/ssl/cert.pem` (macOS, Alpine, BSD)... Updated by the OS package manager instead of by you. Windows has no such file: there, only the environment variables count.
- *Your own files*: to use only what is on the file system, set `use_bundled_cas = false` and give `extra_ca_files` / `extra_ca_dirs`.

HTTP/1.1 only: Beast has no HTTP/2.

A complete program (GET, upload with progress, calling an API from a handler): [`examples/http_client`](examples/http_client/main.cpp).

## WebSocket client

`gtvr::WebSocketClient` (`#include <cppgate/Client/WebSocketClient.h>`) connects to `ws://` and `wss://` servers. One client owns the threads and TLS settings; each `connect()` is a connection.

```cpp
gtvr::WebSocketClient client;      // TLS options as for HttpClient: bundled CAs, use_system_cas, extra_ca_*

auto ws = client.connect(
    {
        .url          = "wss://stream.example.com/v1",
        .headers      = { { "Authorization", "Bearer ..." } },
        .subprotocols = { "v1.json" },
        .reconnect    = { .enabled = true, .initial_delay = std::chrono::milliseconds(500), .max_delay = std::chrono::seconds(30) },
    },
    {
        .onOpen    = [](gtvr::WebSocketClient::Connection& ws) { ws.sendText(R"({"subscribe":"prices"})"); },
        .onMessage = [](gtvr::WebSocketClient::Connection& ws, const gtvr::WebSocketMessage& msg) { /* msg.text() */ },
        .onClose   = [](gtvr::WebSocketClient::Connection& ws, gtvr::WebSocketClient::CloseCode code) { },
        .onError   = [](gtvr::WebSocketClient::Connection& ws, boost::system::error_code ec) { },
        .onReconnecting = [](gtvr::WebSocketClient::Connection& ws, unsigned attempt, std::chrono::milliseconds delay) { },
    });

ws->sendText("hello");          // any thread; false when not connected or the send queue is full
ws->close();                    // no reconnect after this
```

- `onOpen` after every successful connect (subscribe there); `onClose` exactly once for every opened connection.
- A failed attempt (DNS, refused, TLS, timeout) calls `onError`, then `onReconnecting` if a retry follows. Reconnect follows any close you didn't ask for (the server's 1001 on restart included).
- No reconnect after `close()`, nor when the server answers the upgrade with an HTTP status: `onError(client_error::upgrade_rejected)`, the status in `ws->upgradeStatus()`. When `max_attempts` is used up: `onError(client_error::gave_up)`.
- Retries wait `initial_delay`, doubling up to `max_delay`, with ±20% jitter so many clients don't reconnect at the same instant; a successful connect resets it.
- `send*()` return `false` (the message is dropped) when not connected or when `send_queue_limit` messages are waiting: your code decides what to do, the connection stays open.
- Dropping the last `shared_ptr` returned by `connect()` closes the connection. Callbacks receive the connection as a parameter: don't capture its `shared_ptr` in them.
- Destroying the client closes its connections with 1001.

| Option | Default | |
|---|---|---|
| `threads` | 1 | threads running the client and its callbacks |
| `connect_timeout` | 10 s | DNS, TCP, TLS and upgrade of one attempt |
| `max_message_size` | 16 MB | largest message received |
| `idle_timeout`, `keep_alive_pings` | 30 s, on | ping when quiet, treat a silent connection as dead |
| `close_timeout` | 5 s | waiting for the server to answer `close()` |
| `send_queue_limit` | 32 messages | |
| `compression` | off | `permessage-deflate`, if the server agrees |
| TLS options | as `HttpClient` | `verify_peer`, `use_bundled_cas`, `use_system_cas`, `extra_ca_*` |

A complete program (reconnect, sending from another thread, clean Ctrl+C): [`examples/websocket_client`](examples/websocket_client/main.cpp).

## Connection pool

`gtvr::ConnectionPool<T>` (header-only, `#include <cppgate/ConnectionPool.h>`) shares a limited number of connections between threads. `T` is whatever your driver returns; it only has to be movable. With [sqlgen](https://github.com/getml/sqlgen) and PostgreSQL:

```cpp
#include <cppgate/ConnectionPool.h>
#include <sqlgen/postgres.hpp>

using PgConnection = sqlgen::Ref<sqlgen::postgres::Connection>;

const auto credentials = sqlgen::postgres::Credentials{ .user = "app", .password = "...", .host = "localhost", .dbname = "app" };

gtvr::ConnectionPoolOptions options;
options.min_size = 2;
options.max_size = 16;
options.acquire_timeout = std::chrono::seconds(2);

auto pool = gtvr::ConnectionPool<PgConnection>::create(options,
    [credentials] { return sqlgen::postgres::connect(credentials).value(); },      // throws if the database is down
    [](PgConnection& c) { return static_cast<bool>(c->execute("SELECT 1")); });     // optional: still alive?

pool->warmUp();     // opens min_size connections now, fails fast at startup

router->add({ http::verb::get }, "/people", [pool](HttpRequest&, HttpResponse& res) {
    auto conn = pool->acquire();        // waits up to acquire_timeout, throws ConnectionPoolTimeout
    auto people = (sqlgen::read<std::vector<Person>> | sqlgen::limit(100))(conn.get());
    if (!people)
    {
        conn.discard();                 // the connection may be broken: close it, don't reuse it
        return res.send(http::status::service_unavailable, "database error");
    }
    res.sendJSON(rfl::json::write(*people));
});                                     // conn goes back to the pool here
```

- `acquire()` / `acquire(timeout)`: an idle connection (most recently used first), a new one while fewer than `max_size` are open, or else wait in line (FIFO) until one is returned. It throws `ConnectionPoolTimeout` after the timeout, which a handler turns into a 500 if you don't catch it.
- The handle (`gtvr::Pooled<T>`) returns the connection when it goes out of scope; `release()` returns it earlier, `discard()` closes it instead.
- Idle connections are checked with the validate function if unused for `validate_after_idle` (30 s), and closed after `max_idle` (5 min) down to `min_size`. With `max_idle = ConnectionPoolOptions::never` and no validator the pool doesn't read the clock: about 70 ns per acquire + release.
- The factory and validator run outside the pool's lock, from several threads: they must not share unprotected state.
- `close()` wakes waiting threads (they throw `ConnectionPoolClosed`) and closes connections as they come back.

`acquire()` blocks the calling thread while it waits, and so do blocking drivers like libpq while a query runs. cppgate handlers are synchronous, so both happen on an I/O thread: run `io_context::run()` on several threads, and keep `acquire_timeout` short.

## Logging

cppgate logs through spdlog. Call `gtvr::Logger::Init()` once at startup, before creating a listener; the library doesn't initialize it for you. Problems a client can cause (bad requests, disconnects, timeouts) are logged at debug level, so clients can't flood your logs.

## License

[MIT](LICENSE)
