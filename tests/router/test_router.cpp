#include <catch2/catch_test_macros.hpp>

#include <cppgate/Router.h>

#include <atomic>
#include <stdexcept>
#include <mutex>
#include <set>
#include <thread>
#include <string>
#include <vector>

namespace http = boost::beast::http;
using gtvr::router::HttpRequest;
using gtvr::router::HttpResponse;
using gtvr::router::Router;

namespace {

    struct Result
    {
        http::status status;
        std::string body;
        std::string content_type;
    };

    Result call(Router& router, http::verb method, const std::string& target, const std::string& body = {}, const std::string& content_type = {})
    {
        http::request<http::string_body> beast_req{ method, target, 11 };
        if (!content_type.empty())
            beast_req.set(http::field::content_type, content_type);
        beast_req.body() = body;
        beast_req.prepare_payload();

        HttpRequest request(beast_req);
        HttpResponse response;
        router.route(request, response);

        return { response.beast_res_.result(), response.beast_res_.body(), std::string(response.beast_res_[http::field::content_type]) };
    }

    std::string clientIP(Router& router, const std::string& peer, const char* forwarded_for)
    {
        http::request<http::string_body> beast_req{ http::verb::get, "/ip", 11 };
        beast_req.set("X-Forwarded-For", forwarded_for);
        HttpRequest req(beast_req);
        req.setPeerIP(peer);
        HttpResponse res;
        router.route(req, res);
        return res.beast_res_.body();
    }

    struct FakeWebSocket : gtvr::WebSocketContext
    {
        Request req;
        std::vector<std::string> sent;
        std::vector<CloseCode> closed;

        explicit FakeWebSocket(const std::string& target, std::string peer_ip = "127.0.0.1") : WebSocketContext(std::move(peer_ip)), req{ http::verb::get, target, 11 } {}

        const Request& request() const override { return req; }
        void sendText(std::string data) override { sent.push_back(std::move(data)); }
        void sendText(std::shared_ptr<const std::string> data) override { sent.push_back(*data); }
        void sendBinary(std::vector<uint8_t> data) override { sent.emplace_back(data.begin(), data.end()); }
        void close(CloseCode code) override { closed.push_back(code); }
        std::size_t pendingMessages() const override { return 0; }

        // What the real session does on its strand
        using WebSocketContext::emitMessage;
        using WebSocketContext::emitError;
        using WebSocketContext::emitClose;
    };

    struct UpgradeResult
    {
        bool accepted;
        http::status status;
        std::string body;
    };

    UpgradeResult upgrade(Router& router, std::shared_ptr<FakeWebSocket> ws)
    {
        HttpResponse response;
        bool accepted = router.upgrade(ws, response);
        return { accepted, response.beast_res_.result(), response.beast_res_.body() };
    }

}

TEST_CASE("A new HttpResponse has no response yet", "[router]")
{
    HttpResponse response;
    CHECK_FALSE(response.hasResponse());

    response.sendNoContent();
    CHECK(response.hasResponse());
}

TEST_CASE("Static route returns handler response", "[router]")
{
    Router router;
    router.add({ http::verb::get }, "/hello", [](HttpRequest&, HttpResponse& res) {
        res.send("world", "text/plain");
    });

    auto r = call(router, http::verb::get, "/hello");
    CHECK(r.status == http::status::ok);
    CHECK(r.body == "world");
    CHECK(r.content_type == "text/plain");
}

TEST_CASE("Root path", "[router]")
{
    Router router;
    router.add({ http::verb::get }, "/", [](HttpRequest&, HttpResponse& res) {
        res.sendContent("root");
    });

    CHECK(call(router, http::verb::get, "/").body == "root");
    CHECK(call(router, http::verb::get, "/x").status == http::status::not_found);
}

TEST_CASE("Trailing slash is optional", "[router]")
{
    Router router;
    router.add({ http::verb::get }, "/users", [](HttpRequest&, HttpResponse& res) {
        res.sendContent("users");
    });

    CHECK(call(router, http::verb::get, "/users").status == http::status::ok);
    CHECK(call(router, http::verb::get, "/users/").status == http::status::ok);
    CHECK(call(router, http::verb::get, "/users//").status == http::status::not_found);
    CHECK(call(router, http::verb::get, "/usersX").status == http::status::not_found);
}

TEST_CASE("Path parameters are captured", "[router][params]")
{
    Router router;
    router.add({ http::verb::get }, "/users/{id}", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(std::string(req.urlParam("id")));
    });
    router.add({ http::verb::get }, "/api/v1/users/{id}.{format}", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(std::string(req.urlParam("id")) + "|" + std::string(req.urlParam("format")));
    });

    CHECK(call(router, http::verb::get, "/users/42").body == "42");
    CHECK(call(router, http::verb::get, "/users/alice").body == "alice");
    CHECK(call(router, http::verb::get, "/api/v1/users/alice.xml").body == "alice|xml");

    SECTION("a parameter does not span a slash")
    {
        CHECK(call(router, http::verb::get, "/users/1/2").status == http::status::not_found);
    }

    SECTION("an unknown parameter name is empty")
    {
        Router r2;
        r2.add({ http::verb::get }, "/users/{id}", [](HttpRequest& req, HttpResponse& res) {
            res.sendContent(req.urlParam("missing").empty() ? "empty" : "set");
        });
        CHECK(call(r2, http::verb::get, "/users/1").body == "empty");
    }
}

TEST_CASE("Custom parameter regex", "[router][params]")
{
    Router router;
    router.add({ http::verb::get }, R"(/items/{id:\d+})", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(std::string(req.urlParam("id")));
    });

    CHECK(call(router, http::verb::get, "/items/123").body == "123");
    CHECK(call(router, http::verb::get, "/items/abc").status == http::status::not_found);
}

TEST_CASE("Static text in the path is not treated as regex", "[router]")
{
    Router router;
    router.add({ http::verb::get }, "/file.json", [](HttpRequest&, HttpResponse& res) {
        res.sendContent("ok");
    });

    CHECK(call(router, http::verb::get, "/file.json").status == http::status::ok);
    CHECK(call(router, http::verb::get, "/fileXjson").status == http::status::not_found);
}

TEST_CASE("Query string does not affect matching and can be read", "[router][query]")
{
    Router router;
    router.add({ http::verb::get }, "/search", [](HttpRequest& req, HttpResponse& res) {
        auto q = req.urlQueries();
        res.sendContent(q.get("q") + "|" + q.get("page") + "|" + (q.contains("none") ? "yes" : "no"));
    });

    CHECK(call(router, http::verb::get, "/search?q=cpp&page=2").body == "cpp|2|no");
}

TEST_CASE("Form body can be read", "[router][query]")
{
    Router router;
    router.add({ http::verb::post }, "/login", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(req.formQueries().get("user"));
    });

    CHECK(call(router, http::verb::post, "/login", "user=bob&pass=x", "application/x-www-form-urlencoded").body == "bob");
    CHECK(call(router, http::verb::post, "/login", "user=bob&pass=x", "text/plain").body == "");
}

TEST_CASE("Unknown path returns 404", "[router][errors]")
{
    Router router;
    router.add({ http::verb::get }, "/a", [](HttpRequest&, HttpResponse& res) { res.sendContent("a"); });

    CHECK(call(router, http::verb::get, "/b").status == http::status::not_found);

    SECTION("custom not-found handler")
    {
        router.setNotFoundHandler(gtvr::router::HttpHandler([](HttpRequest&, HttpResponse& res) {
            res.send(http::status::not_found, "custom 404");
        }));
        CHECK(call(router, http::verb::get, "/b").body == "custom 404");
    }
}

TEST_CASE("Known path with wrong method returns 405", "[router][errors]")
{
    Router router;
    router.add({ http::verb::get, http::verb::post }, "/users/{id}", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(std::string(req.method_string()));
    });

    CHECK(call(router, http::verb::get, "/users/1").body == "GET");
    CHECK(call(router, http::verb::post, "/users/1").body == "POST");
    CHECK(call(router, http::verb::put, "/users/1").status == http::status::method_not_allowed);

    SECTION("custom method-not-allowed handler")
    {
        router.setMethodNotAllowedHandler(gtvr::router::HttpHandler([](HttpRequest&, HttpResponse& res) {
            res.send(http::status::method_not_allowed, "custom 405");
        }));
        CHECK(call(router, http::verb::delete_, "/users/1").body == "custom 405");
    }
}

TEST_CASE("Same path, different methods, different handlers", "[router]")
{
    Router router;
    router.add({ http::verb::get }, "/users/{id}", [](HttpRequest&, HttpResponse& res) { res.sendContent("get"); });
    router.add({ http::verb::delete_ }, "/users/{id}", [](HttpRequest&, HttpResponse& res) { res.sendContent("delete"); });

    CHECK(call(router, http::verb::get, "/users/1").body == "get");
    CHECK(call(router, http::verb::delete_, "/users/1").body == "delete");
}

TEST_CASE("First registered matching route wins", "[router]")
{
    Router router;
    router.add({ http::verb::get }, "/users/me", [](HttpRequest&, HttpResponse& res) { res.sendContent("me"); });
    router.add({ http::verb::get }, "/users/{id}", [](HttpRequest&, HttpResponse& res) { res.sendContent("id"); });

    CHECK(call(router, http::verb::get, "/users/me").body == "me");
    CHECK(call(router, http::verb::get, "/users/7").body == "id");
}

TEST_CASE("Handler that throws returns 500", "[router][errors]")
{
    Router router;
    router.add({ http::verb::get }, "/boom", [](HttpRequest&, HttpResponse&) {
        throw std::runtime_error("boom");
    });

    CHECK(call(router, http::verb::get, "/boom").status == http::status::internal_server_error);
}

TEST_CASE("Handler that does not respond returns 500", "[router][errors]")
{
    Router router;
    router.add({ http::verb::get }, "/silent", [](HttpRequest&, HttpResponse&) {});

    CHECK(call(router, http::verb::get, "/silent").status == http::status::internal_server_error);
}

TEST_CASE("Pre middleware", "[router][middleware]")
{
    Router router;
    bool handler_called = false;

    gtvr::router::Handler auth = gtvr::router::HttpHandler([](HttpRequest& req, HttpResponse& res) {
        if (req.headers().get(http::field::authorization) != "secret")
            res.sendUnauthorized("no");
    });

    router.add({ http::verb::get }, "/private", { auth }, [&](HttpRequest&, HttpResponse& res) {
        handler_called = true;
        res.sendContent("private");
    });

    SECTION("responding stops the chain")
    {
        auto r = call(router, http::verb::get, "/private");
        CHECK(r.status == http::status::unauthorized);
        CHECK_FALSE(handler_called);
    }

    SECTION("not responding continues to the handler")
    {
        http::request<http::string_body> beast_req{ http::verb::get, "/private", 11 };
        beast_req.set(http::field::authorization, "secret");
        HttpRequest req(beast_req);
        HttpResponse res;
        router.route(req, res);

        CHECK(res.beast_res_.result() == http::status::ok);
        CHECK(handler_called);
    }
}

TEST_CASE("Pre middleware can pass context to the handler", "[router][middleware]")
{
    Router router;

    gtvr::router::Handler set_user = gtvr::router::HttpHandler([](HttpRequest& req, HttpResponse&) {
        req.setContext(std::make_shared<std::string>("alice"));
    });

    router.add({ http::verb::get }, "/me", { set_user }, [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(*req.getContext<std::string>());
    });

    CHECK(call(router, http::verb::get, "/me").body == "alice");
}

TEST_CASE("Post middleware runs only when the handler did not respond", "[router][middleware]")
{
    Router router;
    int post_calls = 0;

    gtvr::router::Handler fallback = gtvr::router::HttpHandler([&](HttpRequest&, HttpResponse& res) {
        ++post_calls;
        res.sendContent("from post");
    });

    router.add({ http::verb::get }, "/responds", [](HttpRequest&, HttpResponse& res) { res.sendContent("handler"); }, { fallback });
    router.add({ http::verb::get }, "/silent", [](HttpRequest&, HttpResponse&) {}, { fallback });

    CHECK(call(router, http::verb::get, "/responds").body == "handler");
    CHECK(post_calls == 0);

    CHECK(call(router, http::verb::get, "/silent").body == "from post");
    CHECK(post_calls == 1);
}

TEST_CASE("Middlewares run in registration order", "[router][middleware]")
{
    Router router;
    std::string trace;

    auto mark = [&](std::string name) {
        return gtvr::router::Handler(gtvr::router::HttpHandler([&trace, name](HttpRequest&, HttpResponse&) { trace += name; }));
    };

    router.add({ http::verb::get }, "/x", { mark("a"), mark("b") }, [&](HttpRequest&, HttpResponse&) { trace += "H"; }, { mark("c"), mark("d") });

    call(router, http::verb::get, "/x");
    CHECK(trace == "abHcd");
}

TEST_CASE("Groups prefix paths and apply middleware", "[router][group]")
{
    Router router;
    std::string trace;

    gtvr::router::Handler log = gtvr::router::HttpHandler([&](HttpRequest&, HttpResponse&) { trace += "L"; });
    gtvr::router::Handler admin_only = gtvr::router::HttpHandler([&](HttpRequest& req, HttpResponse& res) {
        trace += "A";
        if (req.headers().get("X-Role") != "admin")
            res.sendUnauthorized("admins only");
    });

    router.group("/api/v1", { log }, {}, [&](Router& r) {
        r.add({ http::verb::get }, "/users/{id}", [](HttpRequest& req, HttpResponse& res) {
            res.sendContent("user " + std::string(req.urlParam("id")));
        });

        r.group("/admin", { admin_only }, {}, [](Router& r) {
            r.add({ http::verb::get }, "/dashboard", [](HttpRequest&, HttpResponse& res) {
                res.sendContent("dashboard");
            });
        });
    });

    CHECK(call(router, http::verb::get, "/api/v1/users/100").body == "user 100");
    CHECK(trace == "L");

    trace.clear();
    CHECK(call(router, http::verb::get, "/api/v1/admin/dashboard").status == http::status::unauthorized);
    CHECK(trace == "LA");

    CHECK(call(router, http::verb::get, "/users/100").status == http::status::not_found);
    CHECK(call(router, http::verb::get, "/admin/dashboard").status == http::status::not_found);
}

TEST_CASE("A handler can route again while its own params are in use", "[router][params]")
{
    Router router;
    router.add({ http::verb::get }, "/users/{id}", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(std::string(req.urlParam("id")));
    });
    router.add({ http::verb::get }, "/outer/{x}", [&](HttpRequest& req, HttpResponse& res) {
        auto inner = call(router, http::verb::get, "/users/2");
        res.sendContent(std::string(req.urlParam("x")) + inner.body);
    });

    CHECK(call(router, http::verb::get, "/outer/a").body == "a2");
}

TEST_CASE("Routes with more params than the inline capacity", "[router][params]")
{
    static_assert(gtvr::router::RouteParams::inline_capacity == 8);

    Router router;
    router.add({ http::verb::get }, "/{a}/{b}/{c}/{d}/{e}/{f}/{g}/{h}/{i}/{j}", [](HttpRequest& req, HttpResponse& res) {
        std::string out;
        for (auto key : { "a", "b", "c", "d", "e", "f", "g", "h", "i", "j" })
            out += std::string(req.urlParam(key));
        res.sendContent(out);
    });

    CHECK(call(router, http::verb::get, "/0/1/2/3/4/5/6/7/8/9").body == "0123456789");
}

TEST_CASE("Route parameters do not leak between requests", "[router][params]")
{
    Router router;
    router.add({ http::verb::get }, "/a/{x}/{y}", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(std::string(req.urlParam("x")) + std::string(req.urlParam("y")));
    });

    for (int i = 0; i < 100; ++i)
    {
        auto expected = std::to_string(i) + "z";
        REQUIRE(call(router, http::verb::get, "/a/" + std::to_string(i) + "/z").body == expected);
    }
}

TEST_CASE("WebSocket upgrade to an unknown path is refused with 404", "[router][websocket]")
{
    Router router;
    router.add("/ws/chat", [](std::shared_ptr<gtvr::WebSocketContext>) { return true; });

    auto r = upgrade(router, std::make_shared<FakeWebSocket>("/ws/other"));
    CHECK_FALSE(r.accepted);
    CHECK(r.status == http::status::not_found);
}

TEST_CASE("WebSocket routes are found wherever they are registered", "[router][websocket]")
{
    Router router;
    std::string hit;

    router.add("/ws/chat", [&](std::shared_ptr<gtvr::WebSocketContext>) { hit = "chat"; return true; });
    router.add("/ws/news", [&](std::shared_ptr<gtvr::WebSocketContext>) { hit = "news"; return true; });

    auto ws = std::make_shared<FakeWebSocket>("/ws/news");
    REQUIRE(upgrade(router, ws).accepted);
    CHECK(hit.empty());    // the handler is onOpen, it runs only after the handshake

    router.open(ws);
    CHECK(hit == "news");
}

TEST_CASE("HTTP and WebSocket routes on the same path do not mix", "[router][websocket]")
{
    Router router;
    bool hit = false;

    router.add({ http::verb::get }, "/ws", [](HttpRequest&, HttpResponse& res) { res.sendContent("http"); });
    router.add("/ws", [&](std::shared_ptr<gtvr::WebSocketContext>) { hit = true; return true; });

    auto ws = std::make_shared<FakeWebSocket>("/ws");
    REQUIRE(upgrade(router, ws).accepted);
    CHECK_NOTHROW(router.open(ws));
    CHECK(hit);

    CHECK(call(router, http::verb::get, "/ws").body == "http");
}

TEST_CASE("Plain HTTP request to a WebSocket-only path returns 404", "[router][websocket]")
{
    Router router;
    router.add("/ws", [](std::shared_ptr<gtvr::WebSocketContext>) { return true; });

    CHECK(call(router, http::verb::get, "/ws").status == http::status::not_found);
}

TEST_CASE("Pre middleware can refuse the upgrade", "[router][websocket][middleware]")
{
    Router router;
    bool opened = false;

    SECTION("returning false refuses with 403 by default")
    {
        gtvr::router::Handler deny = gtvr::router::WebSocketHandler([](std::shared_ptr<gtvr::WebSocketContext>) { return false; });
        router.add("/ws", { deny }, [&](std::shared_ptr<gtvr::WebSocketContext>) { opened = true; return true; });

        auto ws = std::make_shared<FakeWebSocket>("/ws");
        auto r = upgrade(router, ws);
        CHECK_FALSE(r.accepted);
        CHECK(r.status == http::status::forbidden);

        router.open(ws);
        CHECK_FALSE(opened);
    }

    SECTION("reject() chooses the status and body")
    {
        gtvr::router::Handler auth = gtvr::router::WebSocketHandler([](std::shared_ptr<gtvr::WebSocketContext> ws) {
            if (ws->request()[http::field::authorization] == "secret")
                return true;
            ws->reject(http::status::unauthorized, "login first");
            return false;
        });
        router.add("/ws", { auth }, [&](std::shared_ptr<gtvr::WebSocketContext>) { opened = true; return true; });

        auto r = upgrade(router, std::make_shared<FakeWebSocket>("/ws"));
        CHECK_FALSE(r.accepted);
        CHECK(r.status == http::status::unauthorized);
        CHECK(r.body == "login first");

        auto ok = std::make_shared<FakeWebSocket>("/ws");
        ok->req.set(http::field::authorization, "secret");
        CHECK(upgrade(router, ok).accepted);
    }
}

TEST_CASE("Pre middleware passes user data to the handler and callbacks", "[router][websocket][middleware]")
{
    Router router;
    std::string seen;

    gtvr::router::Handler auth = gtvr::router::WebSocketHandler([](std::shared_ptr<gtvr::WebSocketContext> ws) {
        ws->setContext(std::make_shared<std::string>("alice"));
        return true;
    });

    router.add("/ws", { auth }, [&](std::shared_ptr<gtvr::WebSocketContext> ws) {
        ws->onMessage([&](gtvr::WebSocketContext& ws, const gtvr::WebSocketMessage&) {
            seen = *ws.getContext<std::string>();
        });
        return true;
    });

    auto ws = std::make_shared<FakeWebSocket>("/ws");
    REQUIRE(upgrade(router, ws).accepted);
    router.open(ws);
    ws->emitMessage("hi", true);
    CHECK(seen == "alice");
}

TEST_CASE("WebSocket routes have path parameters", "[router][websocket][params]")
{
    Router router;
    std::string room;

    router.add("/ws/rooms/{room}", [&](std::shared_ptr<gtvr::WebSocketContext> ws) {
        room = std::string(ws->urlParam("room"));
        return true;
    });

    auto ws = std::make_shared<FakeWebSocket>("/ws/rooms/lobby?x=1");
    REQUIRE(upgrade(router, ws).accepted);
    router.open(ws);
    CHECK(room == "lobby");
    CHECK(ws->urlParam("room") == "lobby");    // still valid after the upgrade request is gone
}

TEST_CASE("WebSocket handler runs before post middleware, and false stops the chain", "[router][websocket][middleware]")
{
    Router router;
    std::string trace;

    gtvr::router::Handler post = gtvr::router::WebSocketHandler([&](std::shared_ptr<gtvr::WebSocketContext>) { trace += "P"; return true; });

    router.add("/go", [&](std::shared_ptr<gtvr::WebSocketContext>) { trace += "H"; return true; }, { post });
    router.add("/stop", [&](std::shared_ptr<gtvr::WebSocketContext>) { trace += "H"; return false; }, { post });

    auto go = std::make_shared<FakeWebSocket>("/go");
    REQUIRE(upgrade(router, go).accepted);
    router.open(go);
    CHECK(trace == "HP");

    trace.clear();
    auto stop = std::make_shared<FakeWebSocket>("/stop");
    REQUIRE(upgrade(router, stop).accepted);
    router.open(stop);
    CHECK(trace == "H");
}

TEST_CASE("Exceptions in WebSocket routes", "[router][websocket][errors]")
{
    Router router;

    gtvr::router::Handler boom = gtvr::router::WebSocketHandler([](std::shared_ptr<gtvr::WebSocketContext>) -> bool { throw std::runtime_error("boom"); });

    router.add("/pre", { boom }, [](std::shared_ptr<gtvr::WebSocketContext>) { return true; });
    router.add("/handler", [](std::shared_ptr<gtvr::WebSocketContext>) -> bool { throw std::runtime_error("boom"); });

    SECTION("in a pre middleware: refused with 500")
    {
        auto r = upgrade(router, std::make_shared<FakeWebSocket>("/pre"));
        CHECK_FALSE(r.accepted);
        CHECK(r.status == http::status::internal_server_error);
    }

    SECTION("in the handler: closed with internal_error")
    {
        auto ws = std::make_shared<FakeWebSocket>("/handler");
        REQUIRE(upgrade(router, ws).accepted);
        CHECK_NOTHROW(router.open(ws));
        REQUIRE(ws->closed.size() == 1);
        CHECK(ws->closed[0] == gtvr::WebSocketContext::CloseCode::internal_error);
    }
}

TEST_CASE("open() without an accepted upgrade does nothing", "[router][websocket]")
{
    Router router;
    bool opened = false;
    router.add("/ws", [&](std::shared_ptr<gtvr::WebSocketContext>) { opened = true; return true; });

    router.open(std::make_shared<FakeWebSocket>("/ws"));
    CHECK_FALSE(opened);
}

TEST_CASE("WebSocket callbacks receive the context", "[websocket]")
{
    auto ws = std::make_shared<FakeWebSocket>("/ws");
    std::string error;

    std::vector<uint8_t> bytes;

    ws->onMessage([&](gtvr::WebSocketContext& ws, const gtvr::WebSocketMessage& msg) {
        ws.sendText(std::string(msg.isText() ? "text:" : "bin:") + std::string(msg.text()));
        if (msg.isBinary())
            bytes.assign(msg.bytes().begin(), msg.bytes().end());
    });
    ws->onError([&](gtvr::WebSocketContext&, boost::beast::error_code ec) { error = ec.message(); });

    ws->emitMessage("hello", true);
    ws->emitMessage(std::string_view("\x00\xff", 2), false);
    ws->emitError(boost::asio::error::connection_reset);

    CHECK(ws->sent == std::vector<std::string>{ "text:hello", std::string("bin:\x00\xff", 6) });
    CHECK(bytes == std::vector<uint8_t>{ 0x00, 0xff });
    CHECK_FALSE(error.empty());
}

TEST_CASE("onClose is called exactly once and nothing is called after it", "[websocket]")
{
    auto ws = std::make_shared<FakeWebSocket>("/ws");
    int closes = 0;
    int messages = 0;
    gtvr::WebSocketContext::CloseCode code{};

    ws->onMessage([&](gtvr::WebSocketContext&, const gtvr::WebSocketMessage&) { ++messages; });
    ws->onClose([&](gtvr::WebSocketContext&, gtvr::WebSocketContext::CloseCode c) { ++closes; code = c; });

    ws->emitClose(gtvr::WebSocketContext::CloseCode::going_away);
    ws->emitClose(gtvr::WebSocketContext::CloseCode::abnormal);
    ws->emitMessage("late", true);

    CHECK(closes == 1);
    CHECK(code == gtvr::WebSocketContext::CloseCode::going_away);
    CHECK(messages == 0);
}

TEST_CASE("Callbacks are released on close, so a captured session is not leaked", "[websocket]")
{
    auto ws = std::make_shared<FakeWebSocket>("/ws");
    std::weak_ptr<FakeWebSocket> weak = ws;

    // The cycle the API asks users to avoid, released anyway by emitClose
    ws->onMessage([ws](gtvr::WebSocketContext&, const gtvr::WebSocketMessage&) {});
    ws->emitClose(gtvr::WebSocketContext::CloseCode::normal);

    ws.reset();
    CHECK(weak.expired());
}

TEST_CASE("Groups run only the middlewares matching the route kind", "[router][websocket][group]")
{
    Router router;
    std::string trace;

    gtvr::router::Handler http_mw = gtvr::router::HttpHandler([&](HttpRequest&, HttpResponse&) { trace += "h"; });
    gtvr::router::Handler ws_mw = gtvr::router::WebSocketHandler([&](std::shared_ptr<gtvr::WebSocketContext>) { trace += "w"; return true; });

    router.group("/api", { http_mw, ws_mw }, {}, [](Router& r) {
        r.add({ http::verb::get }, "/status", [](HttpRequest&, HttpResponse& res) { res.sendContent("ok"); });
        r.add("/live", [](std::shared_ptr<gtvr::WebSocketContext>) { return true; });
    });

    CHECK(call(router, http::verb::get, "/api/status").body == "ok");
    CHECK(trace == "h");

    trace.clear();
    auto ws = std::make_shared<FakeWebSocket>("/api/live");
    CHECK(upgrade(router, ws).accepted);
    CHECK(trace == "w");
}

TEST_CASE("Router works out the HTTP client IP through trusted proxies", "[router][ip]")
{
    Router router;
    router.add({ http::verb::get }, "/ip", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(req.getIP() + "|" + req.getPeerIP());
    });

    auto ip_of = [&](const std::string& peer, const char* forwarded_for) {
        http::request<http::string_body> beast_req{ http::verb::get, "/ip", 11 };
        beast_req.set("X-Forwarded-For", forwarded_for);
        HttpRequest req(beast_req);
        req.setPeerIP(peer);
        HttpResponse res;
        router.route(req, res);
        return res.beast_res_.body();
    };

    // Not configured: the header is ignored
    CHECK(ip_of("10.0.0.1", "203.0.113.7") == "10.0.0.1|10.0.0.1");

    router.setTrustedProxies({ "10.0.0.0/8" });
    CHECK(ip_of("10.0.0.1", "203.0.113.7") == "203.0.113.7|10.0.0.1");
    CHECK(ip_of("198.51.100.9", "203.0.113.7") == "198.51.100.9|198.51.100.9");

    SECTION("an invalid list throws and keeps the previous one")
    {
        CHECK_THROWS_AS(router.setTrustedProxies({ "192.168.0.0/16", "nope" }), std::invalid_argument);
        CHECK(ip_of("10.0.0.1", "203.0.113.7") == "203.0.113.7|10.0.0.1");
        CHECK(ip_of("192.168.0.1", "203.0.113.7") == "192.168.0.1|192.168.0.1");
    }
}

TEST_CASE("Router works out the WebSocket client IP through trusted proxies", "[router][websocket][ip]")
{
    Router router;
    router.setTrustedProxies({ "10.0.0.0/8" });
    std::string in_middleware, in_handler;

    gtvr::router::Handler mw = gtvr::router::WebSocketHandler([&](std::shared_ptr<gtvr::WebSocketContext> ws) {
        in_middleware = ws->ip();
        return true;
    });

    router.add("/ws", { mw }, [&](std::shared_ptr<gtvr::WebSocketContext> ws) {
        in_handler = ws->ip() + "|" + ws->peerIP();
        return true;
    });

    auto ws = std::make_shared<FakeWebSocket>("/ws", "10.0.0.1");
    ws->req.set("X-Forwarded-For", "203.0.113.7");
    REQUIRE(upgrade(router, ws).accepted);
    router.open(ws);

    CHECK(in_middleware == "203.0.113.7");
    CHECK(in_handler == "203.0.113.7|10.0.0.1");

    auto direct = std::make_shared<FakeWebSocket>("/ws", "198.51.100.9");
    direct->req.set("X-Forwarded-For", "203.0.113.7");
    REQUIRE(upgrade(router, direct).accepted);
    CHECK(in_middleware == "198.51.100.9");
}

TEST_CASE("setTrustedProxies can be called while requests are routed", "[router][ip][threads]")
{
    Router router;
    router.add({ http::verb::get }, "/ip", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(req.getIP());
    });

    std::atomic<bool> stop{ false };
    std::atomic<int> requests{ 0 };
    std::atomic<int> inconsistent{ 0 };
    std::mutex unexpected_mutex;
    std::set<std::string> unexpected;

    // Catch2 assertions are not thread-safe: workers only count, the main thread checks
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t)
    {
        workers.emplace_back([&] {
            while (!stop)
            {
                auto ip = clientIP(router, "10.0.0.1", "203.0.113.7");

                // Old list (not trusted) or new list (trusted), never anything else
                if (ip != "203.0.113.7" && ip != "10.0.0.1")
                {
                    ++inconsistent;
                    std::lock_guard lock(unexpected_mutex);
                    unexpected.insert(ip);
                }
                ++requests;
            }
        });
    }

    for (int i = 0; i < 2000; ++i)
    {
        if (i % 2)
            router.setTrustedProxies({ "192.168.0.0/16", "10.0.0.0/8", "fd00::/8" });
        else
            router.setTrustedProxies({});
    }

    stop = true;
    for (auto& worker : workers)
        worker.join();

    INFO("unexpected client IPs: " << unexpected.size() << ", e.g. '" << (unexpected.empty() ? "" : *unexpected.begin()) << "'");
    CHECK(unexpected.empty());
    CHECK(inconsistent == 0);
    CHECK(requests > 0);
}

TEST_CASE("Long URLs and large parameter values", "[router][params][big]")
{
    Router router;
    router.add({ http::verb::get }, "/files/{name}", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(std::string(req.urlParam("name")));
    });
    router.add({ http::verb::get }, "/a/{x}/b/{y}", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(std::to_string(req.urlParam("x").size()) + "|" + std::to_string(req.urlParam("y").size()));
    });
    router.add({ http::verb::get }, "/search", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(std::to_string(req.urlQueries().get("q").size()));
    });
    router.add({ http::verb::post }, "/upload", [](HttpRequest& req, HttpResponse& res) {
        res.sendContent(std::to_string(req.body().size()));
    });

    SECTION("a 100 KB path parameter is captured whole")
    {
        std::string name(100 * 1024, 'x');
        CHECK(call(router, http::verb::get, "/files/" + name).body == name);
    }

    SECTION("several large parameters")
    {
        std::string x(50 * 1024, 'x'), y(70 * 1024, 'y');
        CHECK(call(router, http::verb::get, "/a/" + x + "/b/" + y).body == "51200|71680");
    }

    SECTION("percent-encoded parameter is decoded")
    {
        CHECK(call(router, http::verb::get, "/files/hello%20world").body == "hello world");
    }

    SECTION("a large query string")
    {
        std::string q(64 * 1024, 'q');
        CHECK(call(router, http::verb::get, "/search?q=" + q).body == "65536");
    }

    SECTION("a long path that matches no route is a 404")
    {
        CHECK(call(router, http::verb::get, "/" + std::string(100 * 1024, 'z')).status == http::status::not_found);
    }

    SECTION("a 1 MB body reaches the handler (the size limit is the session's job)")
    {
        CHECK(call(router, http::verb::post, "/upload", std::string(1024 * 1024, 'b')).body == "1048576");
    }
}
