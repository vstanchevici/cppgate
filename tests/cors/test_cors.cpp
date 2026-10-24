#include <catch2/catch_test_macros.hpp>

#include <cppgate/Middleware/Cors.h>

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>

namespace http = boost::beast::http;
using gtvr::middleware::CorsOptions;
using gtvr::router::HttpRequest;
using gtvr::router::HttpResponse;
using gtvr::router::Router;

namespace {

    struct Result
    {
        http::status status;
        std::string body;
        http::response<http::string_body> raw;

        std::string header(http::field f) const { return std::string(raw[f]); }
        bool has(http::field f) const { return raw.find(f) != raw.end(); }
    };

    Result call(Router& router, http::verb method, const std::string& target, std::initializer_list<std::pair<http::field, const char*>> headers = {})
    {
        http::request<http::string_body> beast_req{ method, target, 11 };
        for (auto& [field, value] : headers)
            beast_req.set(field, value);

        HttpRequest request(beast_req);
        HttpResponse response;
        router.route(request, response);
        return { response.beast_res_.result(), response.beast_res_.body(), response.beast_res_ };
    }

    struct App
    {
        Router router;
        int handler_calls = 0;

        explicit App(CorsOptions options)
        {
            router.use(gtvr::middleware::cors(options));
            router.add({ http::verb::get, http::verb::post }, "/api/items", [this](HttpRequest&, HttpResponse& res) {
                ++handler_calls;
                res.sendJSON("[]");
            });
        }
    };

    CorsOptions origins(std::initializer_list<const char*> list)
    {
        CorsOptions options;
        options.allowed_origins.assign(list.begin(), list.end());
        return options;
    }

    const auto preflight_headers = {
        std::pair{ http::field::origin, "https://app.example.com" },
        std::pair{ http::field::access_control_request_method, "PUT" },
        std::pair{ http::field::access_control_request_headers, "Content-Type, X-Token" },
    };

}

TEST_CASE("Requests without Origin are untouched", "[cors]")
{
    App app(origins({ "https://app.example.com" }));

    auto r = call(app.router, http::verb::get, "/api/items");
    CHECK(r.status == http::status::ok);
    CHECK_FALSE(r.has(http::field::access_control_allow_origin));
    CHECK(r.header(http::field::vary) == "Origin");     // the answer still depends on the origin
    CHECK(app.handler_calls == 1);
}

TEST_CASE("Allowed origin gets the CORS headers on the route's response", "[cors]")
{
    App app(origins({ "https://app.example.com" }));

    auto r = call(app.router, http::verb::get, "/api/items", { { http::field::origin, "https://app.example.com" } });
    CHECK(r.status == http::status::ok);
    CHECK(r.body == "[]");
    CHECK(r.header(http::field::access_control_allow_origin) == "https://app.example.com");
    CHECK(r.header(http::field::vary) == "Origin");
    CHECK_FALSE(r.has(http::field::access_control_allow_credentials));
    CHECK(app.handler_calls == 1);
}

TEST_CASE("Other origins run the route without CORS headers", "[cors]")
{
    App app(origins({ "https://app.example.com" }));

    auto r = call(app.router, http::verb::get, "/api/items", { { http::field::origin, "https://evil.example.net" } });
    CHECK(r.status == http::status::ok);
    CHECK_FALSE(r.has(http::field::access_control_allow_origin));
    CHECK(app.handler_calls == 1);
}

TEST_CASE("Any origin", "[cors]")
{
    SECTION("without credentials: a fixed *")
    {
        App app(CorsOptions{});
        auto r = call(app.router, http::verb::get, "/api/items", { { http::field::origin, "https://whatever.dev" } });
        CHECK(r.header(http::field::access_control_allow_origin) == "*");
        CHECK_FALSE(r.has(http::field::vary));
    }

    SECTION("with credentials: the origin is echoed, browsers refuse * with credentials")
    {
        CorsOptions options;
        options.allow_credentials = true;
        App app(options);

        auto r = call(app.router, http::verb::get, "/api/items", { { http::field::origin, "https://whatever.dev" } });
        CHECK(r.header(http::field::access_control_allow_origin) == "https://whatever.dev");
        CHECK(r.header(http::field::access_control_allow_credentials) == "true");
        CHECK(r.header(http::field::vary) == "Origin");
    }
}

TEST_CASE("Preflight from an allowed origin is answered before route matching", "[cors][preflight]")
{
    App app(origins({ "https://app.example.com" }));

    // The route has no OPTIONS (or PUT): without CORS this would be a 405
    auto r = call(app.router, http::verb::options, "/api/items", preflight_headers);

    CHECK(r.status == http::status::no_content);
    CHECK(r.header(http::field::access_control_allow_origin) == "https://app.example.com");
    CHECK(r.header(http::field::access_control_allow_methods) == "GET, HEAD, POST, PUT, PATCH, DELETE");
    CHECK(r.header(http::field::access_control_allow_headers) == "Content-Type, X-Token");     // echoed
    CHECK(r.header(http::field::access_control_max_age) == "600");
    CHECK(r.header(http::field::vary).find("Access-Control-Request-Headers") != std::string::npos);
    CHECK(app.handler_calls == 0);
}

TEST_CASE("Preflight from another origin is refused", "[cors][preflight]")
{
    App app(origins({ "https://other.example.com" }));

    auto r = call(app.router, http::verb::options, "/api/items", preflight_headers);
    CHECK(r.status == http::status::forbidden);
    CHECK_FALSE(r.has(http::field::access_control_allow_origin));
    CHECK(app.handler_calls == 0);
}

TEST_CASE("OPTIONS without Access-Control-Request-Method is not a preflight", "[cors][preflight]")
{
    App app(origins({ "https://app.example.com" }));

    auto r = call(app.router, http::verb::options, "/api/items", { { http::field::origin, "https://app.example.com" } });
    CHECK(r.status == http::status::method_not_allowed);      // routed normally
    CHECK(r.header(http::field::access_control_allow_origin) == "https://app.example.com");
}

TEST_CASE("Configured options are used", "[cors]")
{
    CorsOptions options = origins({ "https://app.example.com" });
    options.allowed_methods = { http::verb::get, http::verb::post };
    options.allowed_headers = { "Content-Type", "Authorization" };
    options.exposed_headers = { "X-Total-Count", "ETag" };
    options.allow_credentials = true;
    options.max_age = std::chrono::hours(1);
    App app(options);

    auto pre = call(app.router, http::verb::options, "/api/items", preflight_headers);
    CHECK(pre.header(http::field::access_control_allow_methods) == "GET, POST");
    CHECK(pre.header(http::field::access_control_allow_headers) == "Content-Type, Authorization");
    CHECK(pre.header(http::field::access_control_allow_credentials) == "true");
    CHECK(pre.header(http::field::access_control_max_age) == "3600");

    auto r = call(app.router, http::verb::get, "/api/items", { { http::field::origin, "https://app.example.com" } });
    CHECK(r.header(http::field::access_control_expose_headers) == "X-Total-Count, ETag");
}

TEST_CASE("Error responses keep the CORS headers, so the page can read them", "[cors]")
{
    App app(origins({ "https://app.example.com" }));
    app.router.add({ http::verb::get }, "/boom", [](HttpRequest&, HttpResponse&) { throw std::runtime_error("boom"); });

    auto not_found = call(app.router, http::verb::get, "/nope", { { http::field::origin, "https://app.example.com" } });
    CHECK(not_found.status == http::status::not_found);
    CHECK(not_found.header(http::field::access_control_allow_origin) == "https://app.example.com");

    auto error = call(app.router, http::verb::get, "/boom", { { http::field::origin, "https://app.example.com" } });
    CHECK(error.status == http::status::internal_server_error);
    CHECK(error.header(http::field::access_control_allow_origin) == "https://app.example.com");
}

TEST_CASE("Subdomain patterns", "[cors][origins]")
{
    App app(origins({ "https://*.example.com" }));

    auto allowed = [&](const char* origin) {
        return call(app.router, http::verb::get, "/api/items", { { http::field::origin, origin } }).has(http::field::access_control_allow_origin);
    };

    CHECK(allowed("https://app.example.com"));
    CHECK(allowed("https://a.b.example.com"));
    CHECK_FALSE(allowed("https://example.com"));            // the domain itself is not a subdomain
    CHECK_FALSE(allowed("https://evil-example.com"));
    CHECK_FALSE(allowed("https://example.com.evil.net"));
    CHECK_FALSE(allowed("http://app.example.com"));         // other scheme
    CHECK_FALSE(allowed("https://user@evil.net/.example.com"));
}

TEST_CASE("Invalid origin patterns are rejected at setup", "[cors][origins]")
{
    CHECK_THROWS_AS(gtvr::middleware::cors(origins({ "app.example.com" })), std::invalid_argument);           // no scheme
    CHECK_THROWS_AS(gtvr::middleware::cors(origins({ "https://app.example.com/" })), std::invalid_argument);  // trailing slash never matches
    CHECK_THROWS_AS(gtvr::middleware::cors(origins({ "https://app.*.com" })), std::invalid_argument);
    CHECK_NOTHROW(gtvr::middleware::cors(origins({ "http://localhost:3000", "https://*.example.com", "*" })));
}
