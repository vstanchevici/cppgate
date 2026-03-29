#include <cppgate/Router.h>
#include <cppgate/WebSocketSession.h>

namespace gtvr::router {

    Router::Route::Route(std::set<boost::beast::http::verb> methods, const std::string& path, const std::vector<Handler>& pre_middlewares, Handler handler, const std::vector<Handler>& post_middlewares) :
        methods(methods), path(std::move(path)), handler(handler), post_middlewares(post_middlewares)
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

    bool Router::Route::PathMatch(std::string_view input, RouteParams& params) const
    {      
        // We need N + 1 slots: index 0 is the full match, 1..N are our parameters
        int num_params = static_cast<int>(param_names.size());
        std::vector<std::string_view> captures(num_params + 1);

        if (num_params > 0)
            params.params.clear();

        // We use Match() with ANCHOR_BOTH to ensure the pattern spans the entire URL
        if (pattern->Match(input, 0, input.size(), re2::RE2::ANCHOR_BOTH, captures.data(), captures.size()))
        {
            for (int i = 0; i < num_params; ++i) {
                // captures[0] is the whole string, so we start at index 1
                params.params.push_back({ param_names[i], captures[i + 1] });
                //slog_info("name: {0}, key:{1}", param_names[i], captures[i + 1]);
            }
            return true;
        }

        return false;
    }

    bool Router::Route::MethodMatch(boost::beast::http::verb method) const
    {
        return methods.count(method);
    }

    void Router::Route::operator()(HttpRequest& request, HttpResponse& response) const
    {
        for (auto& middleware : pre_middlewares)
        {
            bool result = std::get<HttpHandler>(middleware)(request, response);
            if (!result)
                break;
        }

        std::get<HttpHandler>(handler)(request, response);

        for (auto& middleware : post_middlewares)
        {
            bool result = std::get<HttpHandler>(middleware)(request, response);            
            if (!result)
                break;
        }
    }        

    void Router::Route::operator()(std::shared_ptr<WebSocketSessionInterface> session) const
    {  
        for (auto& middleware : pre_middlewares)
        {
            bool result = std::get<WebSocketHandler>(middleware)(session);
            if (!result)
                break;
        }

        std::get<WebSocketHandler>(handler)(session);

        for (auto& middleware : post_middlewares)
        {
            bool result = std::get<WebSocketHandler>(middleware)(session);
            if (!result)
                break;
        }
    }

    void Router::setNotFoundHandler(Handler handler) {
        not_found_handler = std::move(handler);
    }

    void Router::setMethodNotAllowedHandler(Handler handler) {
        method_not_allowed_handler = std::move(handler);
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
            {},
            std::move(handler),
            {}
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
            {},
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

    void Router::prepare()
    {
        if ((paramsBufferSize > 0) && (!params_.has_value()))
        {
            params_.emplace(paramsBufferSize);
            std::cout << "ONE TIME" << std::endl;
        }
    }

    void Router::route(HttpRequest& request, HttpResponse& response) const
    {
        bool path_matched = false;

        auto url = boost::urls::parse_uri_reference(request.target());
        
        if (url.has_value())
        {
            auto path = url.value().path();

            for (const auto& route : routes_)
            {
                if (route.PathMatch(path, const_cast<RouteParams&>(params_.value())))
                {
                    path_matched = true;

                    if (route.MethodMatch(request.method()))
                    {
                        route(request, response);
                        return;
                    }
                }
            }

            if (path_matched)
                std::get<HttpHandler>(method_not_allowed_handler)(request, response);   // 405
            else
                std::get<HttpHandler>(not_found_handler)(request, response);            // 404
        }
        else
        {
            std::get<HttpHandler>(not_found_handler)(request, response);                // 404
        }
    }

    void Router::route(std::shared_ptr<WebSocketSessionInterface> session) const
    {
        auto url = boost::urls::parse_uri_reference(session->getRequest().target());

        if (url.has_value())
        {
            auto path = url.value().path();

            for (const auto& route : routes_)
            {
                if (url.has_value())
                {
                    if (route.PathMatch(path, const_cast<RouteParams&>(params_.value())))
                    {
                        route(session);
                        return;
                    }
                }
            }
        }
    }

}//namespace gtvr

