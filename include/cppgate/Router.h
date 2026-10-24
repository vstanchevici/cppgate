#pragma once

#include <array>

#include <cppgate/Log.h>
#include <string>
#include <vector>
#include <set>
#include <functional>
#include <re2/re2.h>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/url/urls.hpp>
#include <boost/url/parse.hpp>
#include <boost/url/parse_query.hpp>
#include <boost/beast/http/rfc7230.hpp>
#include <cppgate/WebSocketContext.h>
#include <cppgate/cppgate.h>
#include <cppgate/TrustedProxies.h>
#include <memory>
#include <atomic>
#include <version>

namespace gtvr::router {

    // Path parameters of the matched route, e.g. {id}. Lives on the stack of Router::route():
    // up to inline_capacity params without any allocation. Values are views into the request path.
    struct RouteParams
    {
        public:
            using Param = std::pair<std::string_view, std::string_view>;

            static constexpr size_t inline_capacity = 8;

        private:
            std::array<Param, inline_capacity>  items_;
            size_t                              count_ = 0;
            std::vector<Param>                  overflow_;      // only for routes with more than inline_capacity params

        public:
            RouteParams() = default;

            void add(std::string_view key, std::string_view value)
            {
                if (count_ < inline_capacity)
                    items_[count_++] = { key, value };
                else
                    overflow_.emplace_back(key, value);
            }

            std::string_view get(std::string_view key) const
            {
                for (size_t i = 0; i < count_; ++i) {
                    if (items_[i].first == key) return items_[i].second;
                }
                for (const auto& [k, v] : overflow_) {
                    if (k == key) return v;
                }
                return {};
            }

            size_t size() const noexcept
            {
                return count_ + overflow_.size();
            }

            template<typename F>
            void forEach(F&& fn) const
            {
                for (size_t i = 0; i < count_; ++i)
                    fn(items_[i].first, items_[i].second);
                for (const auto& [k, v] : overflow_)
                    fn(k, v);
            }

            void reset()
            {
                count_ = 0;
                overflow_.clear();
            }

            // Views into one request: copying them around invites dangling
            RouteParams(const RouteParams&) = delete;
            RouteParams& operator=(const RouteParams&) = delete;
    };
   
    struct HttpRequest
    {
        private:
            const boost::beast::http::request<boost::beast::http::string_body>& request;
            std::string ip;
            std::string peer_ip;
            std::shared_ptr<void> context;

            friend class Router;

        public:
            RouteParams* params;

            void setContext(std::shared_ptr<void> ctx) noexcept {
                context = ctx;
            }

            template<typename T>
            std::shared_ptr<T> getContext() const noexcept {
                return std::static_pointer_cast<T>(context);
            }

            void setIP(std::string ip) noexcept {
                this->ip = std::move(ip);
            }

            // Client IP: the socket address, or the one forwarded by a trusted proxy (see Router::setTrustedProxies)
            std::string& getIP() noexcept {
                return ip;
            }

            // Set by the session, the address of the socket peer (the proxy when behind one)
            void setPeerIP(std::string ip) noexcept {
                peer_ip = std::move(ip);
            }

            const std::string& getPeerIP() const noexcept {
                return peer_ip;
            }

            struct Headers
            {
                private:
                    const boost::beast::http::request<boost::beast::http::string_body>& request;

                public:
                    Headers(const boost::beast::http::request<boost::beast::http::string_body>& request):request(request) {}

                    inline auto begin() const { return request.begin(); }
                    inline auto end() const { return request.end(); }

                    inline std::string_view get(boost::beast::http::field header)
                    {
                        auto it = request.find(header);
                        if (it == request.end())
                            return {};
                        else
                            return it->value();
                    }

                    inline std::string_view get(std::string_view header)
                    {
                        auto it = request.find(header);
                        if (it == request.end())
                            return {};
                        else
                            return it->value();
                    }
            };
        
            struct Queries
            {
                private:
                    boost::urls::params_view params;
                
                public:
                    Queries(boost::urls::params_view params): params(params)
                    {
                    };

                    inline bool empty()
                    {
                        return params.empty();
                    }
                
                    inline auto begin() const { return params.begin(); }
                    inline auto end() const   { return params.end(); }
                
                    inline bool contains(std::string_view name) const
                    {
                        return params.contains(name);
                    }
                
                    inline std::string get(std::string name)
                    {
                        auto iter = params.find(name);

                        if (iter == params.end())
                            return "";

                        return (*iter).value;
                    }
            };
        
        public:
            inline HttpRequest(const boost::beast::http::request<boost::beast::http::string_body>& request):request(request), params(nullptr), context(nullptr)
            {
            }

            inline boost::system::result<boost::urls::url_view> getURLView()
            {
                return boost::urls::parse_uri_reference(request.target());
            }
        
            inline auto headers()
            {
                return Headers(request);
            }
        
            inline auto body()
            {
                return request.body();
            }

            Queries formQueries()
            {
                if (request[boost::beast::http::field::content_type] == "application/x-www-form-urlencoded")
                {
                    auto result = boost::urls::parse_query(request.body());
                    if (result)
                        return Queries(result.value());
                }
                return Queries(boost::urls::params_view{});
            }

            template<bool strict = false>
            Queries urlQueries()
            {
                auto result = (strict)
                    ? boost::urls::parse_origin_form(request.target())
                    : boost::urls::parse_uri_reference(request.target());

                return result ? Queries(result->params()) : Queries({});
            }

            template<bool with_attributes = false>
            void parseHeader(boost::beast::http::field header)
            {
                auto it = request.find(header);
                if (it != request.end())
                {
                    if constexpr (with_attributes)
                    {
                        boost::beast::http::ext_list list{ it->value() };

                        for (auto const& param : list) {
                            // param.first is the encoding (e.g., "gzip")
                            // param.second is the list of attributes (e.g., "q=0.8")
                            //std::cout << "Encoding: " << param.first << "\n";
                            for (auto const& attr : param.second) {
                                //std::cout << "  Attr: " << attr.first << " = " << attr.second << "\n";
                            }
                        }
                    }
                    else
                    {
                        // token_list parses the "gzip, deflate, br" string into iterable tokens
                        boost::beast::http::token_list tokens{ it->value() };

                        for (auto const& token : tokens) {
                            if (token == "gzip") {
                                // Setup compression...
                            }
                        }
                    }
                }
            }

            inline std::string_view urlParam(const std::string& key) const
            {
                return params->get(key);
            }

            inline auto target()
            {
                return request.target();
            }

            inline auto method()
            {
                return request.method();
            }

            inline auto method_string()
            {
                return request.method_string();
            }

            inline auto keepAlive()
            {
                return request.keep_alive();
            }

            inline auto version()
            {
                return request.version();
            }
    };

    struct HttpResponse
    {
        public:
            boost::beast::http::response<boost::beast::http::string_body> beast_res_;;                    

            inline bool keepAlive()
            {
                return beast_res_.keep_alive();
            }
        
            inline void keepAlive(bool keep_alive)
            {
                beast_res_.keep_alive(keep_alive);
            }

            inline void header(boost::beast::http::field name, std::string_view const& value)
            {
                beast_res_.set(name, value);
            }

            inline void header(std::string_view sname, std::string_view const& value)
            {
                beast_res_.set(sname, value);
            }

            inline void version(unsigned int version)
            {
                beast_res_.version(version);
            }
        
            inline bool hasResponse()
            {
                return beast_res_.result_int() != 0;
            }

            /*
            *   Some code status
            *
            *   https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Status/500
            */

            void send(boost::beast::http::status status, const std::string_view content, std::string_view const& content_type)
            {
                beast_res_.set(boost::beast::http::field::content_type, content_type);
                beast_res_.result(status);
                beast_res_.body() = content;
                beast_res_.prepare_payload();
            }

            void send(boost::beast::http::status status, const std::string_view content)
            {
                beast_res_.result(status);
                beast_res_.body() = content;
                beast_res_.prepare_payload();
            }

            void send(const std::string_view content, std::string_view const& content_type)
            {
                beast_res_.set(boost::beast::http::field::content_type, content_type);
                beast_res_.result(boost::beast::http::status::ok);
                beast_res_.body() = content;
                beast_res_.prepare_payload();
            }

            inline void sendNoContent()
            {
                beast_res_.result(boost::beast::http::status::no_content);
            }

            void sendContent(const std::string_view content)
            {
                beast_res_.result(boost::beast::http::status::ok);
                beast_res_.body() = content;
                beast_res_.prepare_payload();
            }

            void sendNotFound(const std::string_view content)
            {
                beast_res_.result(boost::beast::http::status::not_found);
                beast_res_.body() = content;
                beast_res_.prepare_payload();
            }

            void sendUnauthorized(const std::string_view content)
            {
                beast_res_.result(boost::beast::http::status::unauthorized);
                beast_res_.body() = content;
                beast_res_.prepare_payload();
            }

            inline void sendJSON(const std::string_view content)
            {
                send(content, "application/json"); // maybe another function with "text/plain");
            }
    };


    using HttpHandler       = std::function<void(HttpRequest&, HttpResponse&)>;
    using WebSocketHandler  = std::function<bool(std::shared_ptr<WebSocketContext>)>;
    // A route only runs the middlewares of its own kind, so a group can mix HTTP and WebSocket middlewares.
    using Handler           = std::variant<HttpHandler, WebSocketHandler>;
   

    /*
    
    // Usage:
        HeavyObject o1;
        auto o2 = std::make_shared<HeavyObject>();

        auto c1 = HandlerWrapper(&o1);   // Works with pointer
        auto c2 = HandlerWrapper(o1);    // Works by value
        auto c3 = HandlerWrapper(o2);    // Works with shared_ptr

        or use 
        auto c = ToHandler(&o1); //&o1, o1, o2
    */
    template <typename T>
    class HandlerWrapper {
        private:
            T data;

        public:
            // The constructor that allows CTAD to work
            explicit HandlerWrapper(T val) : data(std::move(val)) {}

            template <typename... Args>
            auto operator()(Args&&... args) const {
                if constexpr (std::is_pointer_v<T>) {
                    return (*data)(std::forward<Args>(args)...);
                }
                else if constexpr (requires { data.operator->(); }) {
                    return (*data)(std::forward<Args>(args)...);
                }
                else {
                    return data(std::forward<Args>(args)...);
                }
            }
    };

    template <typename T>
    HandlerWrapper(T) -> HandlerWrapper<T>;


    template <typename T>
    auto ToHandler(T&& val) {
        return HandlerWrapper<std::decay_t<T>>(std::forward<T>(val));
    }

    class CPPGATE_API Router
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

                    bool isWebSocket() const;

                    void operator()(HttpRequest& request, HttpResponse& response) const;

                    // WebSocket: pre-middlewares, before the handshake. False rejects the upgrade.
                    bool upgrade(std::shared_ptr<WebSocketContext> ws) const;

                    // WebSocket: handler (onOpen) and post-middlewares, after the handshake.
                    void open(std::shared_ptr<WebSocketContext> ws) const;
            };

        private:
            Handler not_found_handler = [](HttpRequest& request, HttpResponse& response) {
                response.sendNotFound("Handler not found");
            };

            Handler method_not_allowed_handler = [](HttpRequest& request, HttpResponse& response) {
                response.send(boost::beast::http::status::method_not_allowed , "Method Not Allowed");
            };

            Handler internal_server_error_handler = [](HttpRequest& request, HttpResponse& response) {
                response.send(boost::beast::http::status::internal_server_error, "Internal Server Errord");
            };

            std::vector<Route>                          routes_;

            // Replaced as a whole by setTrustedProxies() while requests read it, so it is swapped atomically
            #ifdef __cpp_lib_atomic_shared_ptr
                std::atomic<std::shared_ptr<const TrustedProxies>> trusted_proxies_{ std::make_shared<const TrustedProxies>() };
            #else
                std::shared_ptr<const TrustedProxies>              trusted_proxies_ = std::make_shared<const TrustedProxies>();    // only through std::atomic_load/atomic_store
            #endif

            std::shared_ptr<const TrustedProxies> trustedProxies() const;
            
        public:        
            void setNotFoundHandler(Handler handler);

            void setMethodNotAllowedHandler(Handler handler);

            // Proxies whose X-Forwarded-For / X-Real-IP headers are believed, e.g. { "127.0.0.1", "10.0.0.0/8" }.
            // Empty by default: the client IP is the socket address. Throws std::invalid_argument.
            // Safe to call at any time, e.g. to refresh CDN ranges: each request uses either the old or the new list.
            void setTrustedProxies(const std::vector<std::string>& cidrs);

            //Group
            void group(const std::string& prefix, const std::vector<Handler>& pre_middlewares, const std::vector<Handler>& post_middlewares, const std::function<void(Router&)>& fn);            
            inline void group(const std::string& prefix, const std::function<void(Router&)>& fn) { group(prefix, {}, {}, fn); }
        
            //WebSocket handlers
            void add(const std::string& path, const std::vector<Handler>& pre_middlewares, WebSocketHandler handler, const std::vector<Handler>& post_middlewares = {});        
            inline void add(const std::string& path, WebSocketHandler handler, const std::vector<Handler>& post_middlewares = {}) { add(path, {}, handler, post_middlewares); }
        
            //HTTP handlers
            void add(std::set<boost::beast::http::verb> methods, const std::string& path, const std::vector<Handler>& pre_middlewares, HttpHandler handler, const std::vector<Handler>& post_middlewares = {});
            inline void add(std::set<boost::beast::http::verb> methods, const std::string& path, HttpHandler handler, const std::vector<Handler>& post_middlewares = {}) { add(methods, path, {}, handler, post_middlewares); }
        
            void route(HttpRequest& request, HttpResponse& response);

            // WebSocket, before the handshake: matches the route and runs its pre-middlewares.
            // Returns false if the upgrade must be refused, `response` then holds the HTTP reply.
            bool upgrade(std::shared_ptr<WebSocketContext> ws, HttpResponse& response);

            // WebSocket, after the handshake: runs the handler and post-middlewares of the route matched by upgrade().
            void open(std::shared_ptr<WebSocketContext> ws);

            // No longer needed: route parameters live on the stack. Kept so existing code compiles.
            [[deprecated("Router::initialize() is not needed anymore, remove the call")]]
            void initialize(size_t buffersCount, size_t bufferSize) {}

    };	

}//namespace gtvr
