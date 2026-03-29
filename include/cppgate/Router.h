#pragma once

#if __has_include(<memory_resource>)
    #include <memory_resource>
    #define HAVE_MEMORY_RESOURCE 1
#endif

#include <cppgate/Log.h>
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <functional>
#include <re2/re2.h>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/url/urls.hpp>
#include <boost/url/parse.hpp>
#include <cppgate/WebSocketSessionInterface.h>
#include <cppgate/cppgate.h>

namespace gtvr::router {

    struct RouteParams
    {
        #ifdef HAVE_MEMORY_RESOURCE
            private:
                std::unique_ptr<std::byte[]> buffer;
            
                std::pmr::monotonic_buffer_resource pool;
        
            public:
                std::pmr::vector<std::pair<std::string_view, std::string_view>> params;

                RouteParams(size_t bufer_size): buffer(std::make_unique<std::byte[]>(bufer_size)), pool(buffer.get(), bufer_size), params(&pool) {}
        #else
            public:
                std::vector<std::pair<std::string_view, std::string_view>> params;

                RouteParams(size_t bufer_size) {}
        #endif


            std::string_view get(std::string_view key) const
            {
                for (const auto& [k, v] : params) {
                    if (k == key) return v;
                }
                return {};
            }
    };
   
    struct HttpRequest
    {
        private:
            const boost::beast::http::request<boost::beast::http::string_body>& request;
            const RouteParams& params;
            
        public:
            inline HttpRequest(const boost::beast::http::request<boost::beast::http::string_body>& request, const RouteParams& params):request(request), params(params)
            {
            }

            inline boost::system::result<boost::urls::url_view> getURLView()
            {
                return boost::urls::parse_uri_reference(request.target());
            }

            inline std::string getQuery(boost::system::result<boost::urls::url_view>& queries, std::string name)
            {
                auto iter = queries.value().params().find(name);

                if (iter == queries.value().params().end())
                    return "";

                return (*iter).value;
            }

            inline std::string_view getParam(const std::string& key) const
            {
                return params.get(key);
            }

            inline auto target()
            {
                return request.target();
            }

            inline auto method()
            {
                return request.method();
            }

            inline auto keep_alive()
            {
                return request.keep_alive();
            }

            inline auto version()
            {
                return request.version();
            }
    };

    struct HttpResponse : public boost::beast::http::response<boost::beast::http::string_body>
    {
        
    };

    using HttpHandler       = std::function<bool(HttpRequest&, HttpResponse&)>;
    using WebSocketHandler  = std::function<bool(std::shared_ptr<WebSocketSessionInterface>)>;
    using Handler           = std::variant<HttpHandler, WebSocketHandler>;
   
    class CPPGATE_EXPORT Router
    {
        private:
        
            class Route
            {
                private:
                    Handler handler;
                    std::unique_ptr<RE2> pattern;
                    std::string path;
                    std::vector<std::string> param_names;
                    std::set<boost::beast::http::verb> methods;
                    std::vector<Handler> pre_middlewares;
                    std::vector<Handler> post_middlewares;

                    friend Router;

                public:
                    void createRegexPattern();

                public:
                    Route(std::set<boost::beast::http::verb> methods, const std::string& path, const std::vector<Handler>& pre_middlewares, Handler handler, const std::vector<Handler>& post_middlewares);

                    bool PathMatch(std::string_view input, RouteParams& params) const;
                    
                    bool MethodMatch(boost::beast::http::verb method) const;

                    void operator()(HttpRequest& request, HttpResponse& response) const;
                    void operator()(std::shared_ptr<WebSocketSessionInterface> session) const;
            };

        private:
            Handler not_found_handler = [](HttpRequest& request, HttpResponse& response) {
                std::cout << "404 Not Found: " << request.target() << "\n";
                return true;
            };

            Handler method_not_allowed_handler = [](HttpRequest& request, HttpResponse& response) {
                std::cout << "405 Method Not Allowed: " << request.target() << "\n";
                return true;
            };

            std::vector<Route> routes_;
            
            size_t paramsBufferSize = 1024;
            std::optional<RouteParams> params_{std::nullopt};

        public:
            inline const RouteParams& getRouteParamsObject() { return params_.value(); }
        
            void setNotFoundHandler(Handler handler);

            void setMethodNotAllowedHandler(Handler handler);

            //Group
            void group(const std::string& prefix, const std::vector<Handler>& pre_middlewares, const std::vector<Handler>& post_middlewares, const std::function<void(Router&)>& fn);            
            inline void group(const std::string& prefix, const std::function<void(Router&)>& fn) { group(prefix, {}, {}, fn); }
        
            //WebSocket handlers
            void add(const std::string& path, const std::vector<Handler>& pre_middlewares, WebSocketHandler handler, const std::vector<Handler>& post_middlewares = {});        
            inline void add(const std::string& path, WebSocketHandler handler, const std::vector<Handler>& post_middlewares = {}) { add(path, {}, handler, post_middlewares); }
        
            //HTTP handlers
            void add(std::set<boost::beast::http::verb> methods, const std::string& path, const std::vector<Handler>& pre_middlewares, HttpHandler handler, const std::vector<Handler>& post_middlewares = {});
            inline void add(std::set<boost::beast::http::verb> methods, const std::string& path, HttpHandler handler, const std::vector<Handler>& post_middlewares = {}) { add(methods, path, {}, handler, post_middlewares); }
        
            void route(HttpRequest& request, HttpResponse& response) const;
            void route(std::shared_ptr<WebSocketSessionInterface> session) const;

            void prepare();

    };	

}//namespace gtvr
