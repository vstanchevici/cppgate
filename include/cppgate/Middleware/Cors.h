#pragma once

#include <cppgate/Router.h>
#include <cppgate/cppgate.h>
#include <chrono>
#include <string>
#include <vector>

namespace gtvr::middleware {

    struct CorsOptions
    {
        // Exact origins ("https://app.example.com"), subdomains ("https://*.example.com", not the
        // domain itself) or "*" for any origin
        std::vector<std::string>                    allowed_origins     = { "*" };

        std::vector<boost::beast::http::verb>       allowed_methods     = {
            boost::beast::http::verb::get, boost::beast::http::verb::head, boost::beast::http::verb::post,
            boost::beast::http::verb::put, boost::beast::http::verb::patch, boost::beast::http::verb::delete_
        };

        // Request headers the browser may send. Empty: allow whatever the preflight asks for.
        std::vector<std::string>                    allowed_headers;

        // Response headers JavaScript may read, beyond the always-visible simple ones
        std::vector<std::string>                    exposed_headers;

        // Cookies / Authorization. With "*" the request's origin is echoed instead, browsers refuse
        // "*" together with credentials.
        bool                                        allow_credentials   = false;

        // How long the browser may cache a preflight
        std::chrono::seconds                        max_age             { 600 };
    };

    /*
        Cross-Origin Resource Sharing. Use it with Router::use(), so preflight (OPTIONS) requests are
        answered before route matching, whatever methods the route has:

            gtvr::middleware::CorsOptions cors;
            cors.allowed_origins = { "https://app.example.com" };
            router->use(gtvr::middleware::cors(cors));

        - No Origin header (same origin, curl, servers): nothing changes.
        - Allowed origin: the CORS headers are added to the response, including 404/405/500.
        - Preflight from an allowed origin: 204 with the allowed methods/headers, handlers don't run.
        - Preflight from another origin: 403. Other requests from it run without CORS headers,
          so the browser doesn't let the page read the response.

        CORS is enforced by browsers only: it is not access control, protect your routes anyway.
        Throws std::invalid_argument for a malformed origin pattern.
    */
    CPPGATE_API router::HttpHandler cors(CorsOptions options = {});

}//namespace gtvr::middleware
