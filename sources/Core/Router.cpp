#include <cppgate/Router.h>

namespace gtvr::router {

    Router::Route::Route(std::set<boost::beast::http::verb> methods, const std::string& path, const std::vector<Handler>& pre_middlewares, Handler handler, const std::vector<Handler>& post_middlewares) :
        methods(methods), path(std::move(path)), handler(handler), pre_middlewares(pre_middlewares), post_middlewares(post_middlewares)
    {
        createRegexPattern();
    }

    void Router::Route::createRegexPattern()
    {
        std::string regex_pattern = "^";

        // Use StringPiece for efficient parsing of the 'path' string
        std::string_view input(path);
        std::string_view prefix, name, custom_re;

        /**
         * Parsing Logic:
         * 1. Find everything BEFORE the first '{' (the prefix)
         * 2. Find the content INSIDE the '{'
         * - Format: {name} or {name:regex}
         */
        while (re2::RE2::Consume(&input, "([^{]*)", &prefix)) {
            // 1. Add static text (e.g., "/users/") to the regex, escaping special chars
            regex_pattern += re2::RE2::QuoteMeta(prefix);

            // 2. Try to consume a parameter block: {name} or {name:custom_re}
            // This looks for '{', then alpha-numeric name, then optional ':' and regex, then '}'
            if (re2::RE2::Consume(&input, "\\{([a-zA-Z0-9_]+)(?::([^}]+))?\\}", &name, &custom_re)) {

                // Save the name for later mapping (e.g., "id")
                param_names.push_back(std::string(name));

                if (custom_re.empty()) {
                    // Case: {id} -> Match anything that isn't a slash
                    regex_pattern += "([^/]+)";
                }
                else {
                    // Case: {id:\d+} -> Use the user's custom regex
                    // Wrap in () to create a capture group for RE2
                    regex_pattern += "(" + std::string(custom_re) + ")";
                }
            }
            else {
                // No more brackets found
                break;
            }
        }

        // Allow an optional trailing slash and ensure it matches the end of the string
        regex_pattern += "/?$";

        // Create the regex pattern
        pattern = std::make_unique<RE2>(regex_pattern);
    }

    bool Router::Route::pathMatch(std::string_view input, RouteParams& params) const
    {      
        // We need N + 1 slots: index 0 is the full match, 1..N are our parameters
        int num_params = static_cast<int>(param_names.size());

        // On the stack for usual routes, the heap only for routes with many params
        std::array<std::string_view, RouteParams::inline_capacity + 1> inline_captures;
        std::vector<std::string_view> heap_captures;
        std::string_view* captures = inline_captures.data();
        if (num_params > static_cast<int>(RouteParams::inline_capacity))
        {
            heap_captures.resize(num_params + 1);
            captures = heap_captures.data();
        }

        params.reset();

        // We use Match() with ANCHOR_BOTH to ensure the pattern spans the entire URL
        if (pattern->Match(input, 0, input.size(), re2::RE2::ANCHOR_BOTH, captures, num_params + 1))
        {
            for (int i = 0; i < num_params; ++i) {
                // captures[0] is the whole string, so we start at index 1
                params.add(param_names[i], captures[i + 1]);
            }
            return true;
        }

        return false;
    }

    bool Router::Route::methodMatch(boost::beast::http::verb method) const
    {
        return methods.count(method);
    }

    bool Router::Route::isWebSocket() const
    {
        return std::holds_alternative<WebSocketHandler>(handler);
    }

    void Router::Route::operator()(HttpRequest& request, HttpResponse& response) const
    {
        for (auto& middleware : pre_middlewares)
        {
            if (auto* fn = std::get_if<HttpHandler>(&middleware))
            {
                (*fn)(request, response);
                if (response.hasResponse())
                    break;
            }
        }

        if (!response.hasResponse())
        {
            std::get<HttpHandler>(handler)(request, response);

            if (!response.hasResponse())
            {
                for (auto& middleware : post_middlewares)
                {
                    if (auto* fn = std::get_if<HttpHandler>(&middleware))
                    {
                        (*fn)(request, response);
                        if (response.hasResponse())
                            break;
                    }
                }
            }
        }
    }

    bool Router::Route::upgrade(std::shared_ptr<WebSocketContext> ws) const
    {
        for (auto& middleware : pre_middlewares)
        {
            if (auto* fn = std::get_if<WebSocketHandler>(&middleware))
            {
                if (!(*fn)(ws))
                    return false;
            }
        }

        return true;
    }

    void Router::Route::open(std::shared_ptr<WebSocketContext> ws) const
    {
        if (!std::get<WebSocketHandler>(handler)(ws))
            return;

        for (auto& middleware : post_middlewares)
        {
            if (auto* fn = std::get_if<WebSocketHandler>(&middleware))
            {
                if (!(*fn)(ws))
                    break;
            }
        }
    }

    void Router::setNotFoundHandler(Handler handler) {
        not_found_handler = std::move(handler);
    }

    void Router::setMethodNotAllowedHandler(Handler handler) {
        method_not_allowed_handler = std::move(handler);
    }

    void Router::use(HttpHandler middleware)
    {
        global_middlewares_.push_back(std::move(middleware));
    }

    void Router::setTrustedProxies(const std::vector<std::string>& cidrs)
    {
        // Built completely before being published, a request never sees a half-filled list
        auto proxies = std::make_shared<TrustedProxies>();
        for (const auto& cidr : cidrs)
            proxies->add(cidr);

        std::shared_ptr<const TrustedProxies> published = std::move(proxies);

        #ifdef __cpp_lib_atomic_shared_ptr
            trusted_proxies_.store(std::move(published));
        #else
            std::atomic_store(&trusted_proxies_, std::move(published));
        #endif
    }

    std::shared_ptr<const TrustedProxies> Router::trustedProxies() const
    {
        #ifdef __cpp_lib_atomic_shared_ptr
            return trusted_proxies_.load();
        #else
            return std::atomic_load(&trusted_proxies_);
        #endif
    }

    void Router::group(const std::string& prefix, const std::vector<Handler>& pre_middlewares, const std::vector<Handler>& post_middlewares, const std::function<void(Router&)>& fn)
    {
        Router subrouter;

        // Register routes in subrouter
        fn(subrouter);

        // Prefix the paths and apply middleware for each route
        for (auto& route : subrouter.routes_)
        {
            // Combine post group middleware with individual route middleware
            std::vector<Handler> pre_combined_middlewares = pre_middlewares;
            pre_combined_middlewares.insert(pre_combined_middlewares.end(), route.pre_middlewares.begin(), route.pre_middlewares.end());

            std::vector<Handler> post_combined_middlewares = post_middlewares;
            post_combined_middlewares.insert(post_combined_middlewares.end(), route.post_middlewares.begin(),route.post_middlewares.end());

            // Create a new route with prefixed path
            std::string full_path = prefix + route.path;

            if (std::holds_alternative<HttpHandler>(route.handler))
                add(route.methods, full_path, pre_combined_middlewares, std::get<HttpHandler>(route.handler), post_combined_middlewares);
            else
            if (std::holds_alternative<WebSocketHandler>(route.handler))
                add(full_path, pre_combined_middlewares, std::get<WebSocketHandler>(route.handler), post_combined_middlewares);

        }
    }

    void Router::add(const std::string& path, const std::vector<Handler>& pre_middlewares, WebSocketHandler handler, const std::vector<Handler>& post_middlewares)
    {
        // Create the route entry
        auto entry = Route{
            {},
            std::move(path),
            std::move(pre_middlewares),
            std::move(handler),
            std::move(post_middlewares)
        };

        // Safety check: Ensure the regex compiled successfully
        if (!entry.pattern->ok()) {
            // Throw an exception or log an error here
            return;
        }

        routes_.push_back(std::move(entry));
    }

    void Router::add(std::set<boost::beast::http::verb> methods, const std::string& path, const std::vector<Handler>& pre_middlewares, HttpHandler handler, const std::vector<Handler>& post_middlewares)
    {
        // Create the route entry
        auto entry = Route{
            std::move(methods),
            std::move(path),
            std::move(pre_middlewares),
            std::move(handler),
            std::move(post_middlewares)
        };

        // Safety check: Ensure the regex compiled successfully
        if (!entry.pattern->ok()) {
            // Throw an exception or log an error here
            return; 
        }

        routes_.push_back(std::move(entry));
    }

    void Router::route(HttpRequest& request, HttpResponse& response)
    {
        bool path_matched = false;

        request.setIP(trustedProxies()->resolve(request.getPeerIP(), request.request));

        for (const auto& middleware : global_middlewares_)
        {
            try {
                middleware(request, response);
            }
            catch (...)
            {
                std::get<HttpHandler>(internal_server_error_handler)(request, response);        // 500
                return;
            }

            if (response.hasResponse())
                return;
        }

        auto url = boost::urls::parse_uri_reference(request.target());
        
        if (url.has_value())
        {
            auto path = url.value().path();

            // On the stack: owned by this call only, nothing shared between threads or requests
            RouteParams params;
            request.params = &params;

            {

                for (const auto& route : routes_)
                {
                    if (route.isWebSocket())
                        continue;

                    if (route.pathMatch(path, *request.params))
                    {
                        path_matched = true;

                        if (route.methodMatch(request.method()))
                        {
                            bool internal_server_error = false;

                            try {
                                route(request, response);

                                internal_server_error = response.hasResponse() == false;
                            }
                            catch (...)
                            {
                                internal_server_error = true;
                            }

                            if (internal_server_error)
                                std::get<HttpHandler>(internal_server_error_handler)(request, response);        // 500

                            // params dies with this call, don't leave a dangling pointer
                            request.params = nullptr;

                            return;
                        }
                    }
                }

                if (path_matched)
                    std::get<HttpHandler>(method_not_allowed_handler)(request, response);   // 405
                else
                    std::get<HttpHandler>(not_found_handler)(request, response);            // 404

                request.params = nullptr;
            }
        }
        else
        {
            std::get<HttpHandler>(internal_server_error_handler)(request, response);        // 500
        }
    }

    bool Router::upgrade(std::shared_ptr<WebSocketContext> ws, HttpResponse& response)
    {
        ws->ip_ = trustedProxies()->resolve(ws->peer_ip_, ws->request());

        HttpRequest request(ws->request());
        request.setPeerIP(ws->peer_ip_);
        request.setIP(ws->ip_);

        auto url = boost::urls::parse_uri_reference(ws->request().target());

        if (!url.has_value())
        {
            std::get<HttpHandler>(internal_server_error_handler)(request, response);        // 500
            return false;
        }

        auto path = url.value().path();

        RouteParams params;

        for (size_t i = 0; i < routes_.size(); ++i)
        {
            const auto& route = routes_[i];

            if (!route.isWebSocket() || !route.pathMatch(path, params))
                continue;

            // The connection outlives the request, so it keeps its own copy of the params
            ws->params_.clear();
            params.forEach([&ws](std::string_view key, std::string_view value) {
                ws->params_.emplace_back(std::string(key), std::string(value));
            });

            bool accepted = false;

            try {
                accepted = route.upgrade(ws);
            }
            catch (...)
            {
                std::get<HttpHandler>(internal_server_error_handler)(request, response);    // 500
                return false;
            }

            if (!accepted)
            {
                if (ws->reject_status_ == boost::beast::http::status::unknown)
                    response.send(boost::beast::http::status::forbidden, "Forbidden");     // 403
                else
                    response.send(ws->reject_status_, ws->reject_body_);

                return false;
            }

            ws->route_index_ = i;
            return true;
        }

        std::get<HttpHandler>(not_found_handler)(request, response);                        // 404
        return false;
    }

    void Router::open(std::shared_ptr<WebSocketContext> ws)
    {
        if (ws->route_index_ >= routes_.size())
            return;

        try {
            routes_[ws->route_index_].open(ws);
        }
        catch (...)
        {
            ws->close(WebSocketContext::CloseCode::internal_error);
        }
    }

}//namespace gtvr

