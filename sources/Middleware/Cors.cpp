#include <cppgate/Middleware/Cors.h>

#include <memory>
#include <stdexcept>

namespace gtvr::middleware {

    namespace {

        namespace http = boost::beast::http;

        std::string join(const std::vector<std::string>& items)
        {
            std::string out;
            for (const auto& item : items)
            {
                if (!out.empty())
                    out += ", ";
                out += item;
            }
            return out;
        }

        // Compiled once, shared by every request
        struct Policy
        {
            bool                        any_origin = false;
            std::vector<std::string>    exact;
            std::vector<std::string>    subdomain_prefixes;     // "https://"
            std::vector<std::string>    subdomain_suffixes;     // ".example.com"

            std::string                 methods;
            std::string                 headers;                // empty: echo the preflight's request
            std::string                 exposed;
            std::string                 max_age;
            bool                        credentials = false;

            bool allows(std::string_view origin) const
            {
                if (any_origin)
                    return true;

                for (const auto& o : exact)
                    if (origin == o)
                        return true;

                for (size_t i = 0; i < subdomain_prefixes.size(); ++i)
                {
                    const auto& prefix = subdomain_prefixes[i];
                    const auto& suffix = subdomain_suffixes[i];

                    // At least one character of subdomain between them
                    if (origin.size() > prefix.size() + suffix.size()
                        && origin.substr(0, prefix.size()) == prefix
                        && origin.substr(origin.size() - suffix.size()) == suffix)
                    {
                        auto label = origin.substr(prefix.size(), origin.size() - prefix.size() - suffix.size());
                        if (label.find_first_of("/:@") == std::string_view::npos)
                            return true;
                    }
                }

                return false;
            }
        };

        std::shared_ptr<const Policy> compile(const CorsOptions& options)
        {
            auto policy = std::make_shared<Policy>();

            for (const auto& origin : options.allowed_origins)
            {
                if (origin == "*")
                {
                    policy->any_origin = true;
                    continue;
                }

                auto wildcard = origin.find("://*.");
                if (wildcard != std::string::npos)
                {
                    policy->subdomain_prefixes.push_back(origin.substr(0, wildcard + 3));
                    policy->subdomain_suffixes.push_back(origin.substr(wildcard + 4));
                    continue;
                }

                if (origin.find('*') != std::string::npos || origin.find("://") == std::string::npos || origin.back() == '/')
                    throw std::invalid_argument("cors: invalid origin '" + origin + "', expected e.g. https://app.example.com, https://*.example.com or *");

                policy->exact.push_back(origin);
            }

            std::vector<std::string> methods;
            for (auto verb : options.allowed_methods)
                methods.emplace_back(http::to_string(verb));

            policy->methods     = join(methods);
            policy->headers     = join(options.allowed_headers);
            policy->exposed     = join(options.exposed_headers);
            policy->max_age     = std::to_string(options.max_age.count());
            policy->credentials = options.allow_credentials;

            return policy;
        }

    }

    router::HttpHandler cors(CorsOptions options)
    {
        auto policy = compile(options);

        return [policy](router::HttpRequest& req, router::HttpResponse& res)
        {
            // A fixed "*" is the same for everyone; otherwise caches must keep one answer per origin,
            // including for requests from other origins or without one
            bool varies = !(policy->any_origin && !policy->credentials);
            if (varies)
                res.header(http::field::vary, "Origin");

            auto headers = req.headers();
            std::string_view origin = headers.get(http::field::origin);

            // Not a cross-origin browser request
            if (origin.empty())
                return;

            bool preflight = req.method() == http::verb::options
                && !headers.get(http::field::access_control_request_method).empty();

            if (!policy->allows(origin))
            {
                if (preflight)
                    res.send(http::status::forbidden, "CORS origin not allowed", "text/plain");
                return;
            }

            // "*" can't be used with credentials: echo the origin instead
            res.header(http::field::access_control_allow_origin, varies ? origin : std::string_view("*"));

            if (policy->credentials)
                res.header(http::field::access_control_allow_credentials, "true");

            if (!preflight)
            {
                if (!policy->exposed.empty())
                    res.header(http::field::access_control_expose_headers, policy->exposed);
                return;     // continue to the route, the headers stay on its response
            }

            res.header(http::field::access_control_allow_methods, policy->methods);

            if (!policy->headers.empty())
                res.header(http::field::access_control_allow_headers, policy->headers);
            else if (auto requested = headers.get(http::field::access_control_request_headers); !requested.empty())
                res.header(http::field::access_control_allow_headers, requested);

            res.header(http::field::access_control_max_age, policy->max_age);
            res.header(http::field::vary, "Origin, Access-Control-Request-Method, Access-Control-Request-Headers");

            res.sendNoContent();
        };
    }

}//namespace gtvr::middleware
