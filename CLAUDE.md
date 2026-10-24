# cppgate

C++20 REST & WebSocket library built on Boost.Beast/Asio (v0.1.0). Ships as a static (default) or shared library; namespace `gtvr` (router lives in `gtvr::router`).

## Build

Dependencies come from Conan 2 (`conan/conanfile.py`): boost 1.86, re2, openssl 3.x, spdlog (header-only).

macOS (Xcode generator, output in `build/xcode`):
```sh
cd conan && ./BuildMacOS.sh Debug        # conan install + `conan editable add`
cd .. && cmake -S . -G Xcode -B build/xcode \
  -DCMAKE_TOOLCHAIN_FILE=build/xcode/generators/conan_toolchain.cmake \
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
cmake --build build/xcode --config Debug
```
Windows: `conan/BuildVS2022.bat`, then the same with `-G "Visual Studio 17 2022" -B build/vs2022`. Linux: `conan/BuildLinux.sh`.

Tests (Catch2 comes from the conanfile as a `test_requires`):
```sh
cmake build/xcode -DBUILD_TESTING=ON
cmake --build build/xcode --config Debug --target test_router
./build/xcode/Debug/test_router            # also test_trusted_proxies, test_ring_buffer; or: ctest --test-dir build/xcode -C Debug
```
ThreadSanitizer build (Ninja, separate folder, reuses the Conan toolchain):
```sh
cmake -S . -B build/tsan -G Ninja -DCMAKE_TOOLCHAIN_FILE=build/xcode/generators/conan_toolchain.cmake \
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/tsan && ./build/tsan/test_router "[threads]"
```

CMake options: `BUILD_TESTING` (OFF), `BUILD_EXAMPLES` (OFF; each `examples/*` dir is added), `BUILD_SHARED_LIBS`.

`examples/echo` is an HTTP + WebSocket echo server (`./build/xcode/Debug/echo_server [port] [cert.pem key.pem [tls-only]]`; with a certificate it serves HTTP+HTTPS on one port, or HTTPS only). The library build alone does not instantiate the session templates (`HttpSession`, `WebSocketSession`, `Listener::Run`), so build the example to check they compile.

## Layout

- `include/cppgate/` — public headers. Much of the logic is header-only templates (`Listener`, `HttpSession`, `WebSocketSession`). `RingBuffer.h` (lock-free MPMC queue) is currently unused, kept for later.
- `sources/Core/` — `Router.cpp`, `Log.cpp`.
- `sources/BoostBeast/` — session/listener translation units.
- CMake also globs `sources/{Authentication,Handlers}` and the matching include dirs; they don't exist yet but are where new modules are expected to go. New `.cpp`/`.h` files in these dirs are picked up automatically (`find_source_files` in `cmake/Tools.cmake`) — re-run CMake configure after adding files.
- `tests/<name>/` — each subdir is its own CMake project using Catch2.

## Architecture

- `Listener` accepts TCP connections; `Run<flags>()` takes a `Protocol` bitmask (`HTTP | HTTPS | WS | WSS`) resolved at compile time, and spawns `HttpSession<plain, ssl>` per connection on its own strand.
  - With plain support the session starts plain and `async_detect_ssl` switches to TLS when HTTPS is enabled (HTTP and HTTPS on one port); HTTPS alone does the handshake from the first byte. A TLS client on a plain-only listener is closed at once.
  - Any SSL flag needs `SetSSLContext()` before `Run()` (throws `std::logic_error` otherwise); the context must outlive the Listener. The WS/WSS flags change nothing today: upgrades are accepted on any listener.
- `Router` matches routes by regex (RE2) built from paths like `/users/{id}`; supports method sets, groups with prefixes, and pre/post middlewares. A `Handler` is `std::variant<HttpHandler, WebSocketHandler>`.
- `RouteParams` lives on the stack of `Router::route()`: a fixed array of 8 `string_view` pairs, heap only beyond that. Values are views into the request path — don't outlive the request. `Router::initialize()` is a deprecated no-op.
- `HttpRequest`/`HttpResponse` are thin wrappers over Beast's `request/response<string_body>`.
- WebSocket routes (`router.add(path, pre, handler, post)`) work in two phases, with `WebSocketContext` (implemented by `WebSocketSession`) as the per-connection object:
  1. `Router::upgrade()` runs **before** the handshake: path match + pre-middlewares. Refusing (return false, optionally `ws->reject(status, body)`) sends an HTTP response instead of upgrading.
  2. `Router::open()` runs after the handshake: the handler is the onOpen event, where `onMessage/onClose/onError` are set (`onMessage` gets a `WebSocketMessage`: `isText()`, `text()`, `bytes()`, a view valid only during the callback). Reading starts only after it returns.
  Callbacks receive `WebSocketContext&` so users never capture the `shared_ptr` (cycle). `onClose` fires exactly once (guarded in `emitClose`) and releases all callbacks.
- Client IP: sessions only set the socket address (`HttpRequest::setPeerIP`, `WebSocketContext` ctor); the Router works it out per request with `TrustedProxies` (`Router::setTrustedProxies`, empty by default, safe to call while serving: the list is an immutable `shared_ptr` swapped atomically). `X-Forwarded-For` is read right to left and only from trusted peers, then `X-Real-IP`. `getIP()`/`ip()` = client, `getPeerIP()`/`peerIP()` = socket.
- HTTP routing skips WebSocket routes and vice versa; routes only run middlewares of their own kind (`std::get_if`), so groups can mix both.
- Logging: use the `slog_trace/debug/info/warn/error/critical` macros from `Log.h` (spdlog, fmt-style format strings).
- Log level rule: anything a client can cause (bad/oversized requests, disconnects, timeouts) is `slog_debug`, never error, otherwise clients can flood the logs.
- `ServerOptions` (`Listener::SetOptions`, before `Run`) holds the HTTP header/body limits, timeouts and WebSocket limits; sessions keep a copy. Parser errors get a real response (400, 413 body, 414 URL, 431 headers) with `Connection: close`, then a lingering close (`http_linger_timeout`) that drains the rest of the request so the client reads the response instead of a reset (plain and TLS; over TLS it drains instead of sending close_notify).

## Conventions

- 4-space indentation, `access:` labels indented inside classes, braces on their own line for functions/control flow.
- Public classes are exported with `CPPGATE_API` (`cppgate.h`); it's defined empty for static builds.
- Methods: mostly camelCase (`setRouter`, `sendJSON`); some older ones are PascalCase (`Run`, `SetSSLContext`, `OnMessage`). Match the surrounding class.
- Members use a trailing underscore (`routes_`, `router_`).

## Testing notes

- Router tests need no network: build a Beast `request`, wrap it in `HttpRequest`, call `router.route(req, res)`, inspect `res.beast_res_`. See `tests/router/test_router.cpp`.
- No setup needed before `route()`. If `RingBuffer` is used again: `pop()` can fail while items are free (a concurrent push in progress), never treat a failed pop as an error.
- A fresh `HttpResponse` must get `beast_res_.result(0)` before routing — Beast defaults to 200, and `hasResponse()` treats non-zero as "already answered". `HttpSession` does this reset; tests must too.
- Link only `Boost::headers Boost::url`, never `boost::boost`: it pulls in Boost.Test's `main()`, which clashes with Catch2.
