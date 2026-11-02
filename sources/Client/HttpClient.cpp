#include <cppgate/Client/HttpClient.h>

#include "ClientCommon.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/url/parse.hpp>
#include <algorithm>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <variant>

namespace gtvr {

    namespace {

        class ClientCategory : public boost::system::error_category
        {
            public:
                const char* name() const noexcept override { return "cppgate.client"; }

                std::string message(int value) const override
                {
                    switch (static_cast<client_error>(value))
                    {
                        case client_error::timeout:                 return "request timed out";
                        case client_error::cancelled:               return "request cancelled";
                        case client_error::response_too_large:      return "response larger than max_response_size";
                        case client_error::invalid_url:             return "invalid URL";
                        case client_error::unsupported_scheme:      return "unsupported URL scheme, only http and https";
                        case client_error::closed:                  return "HTTP client destroyed";
                        case client_error::wait_on_client_thread:   return "Call::wait() from a client callback would deadlock";
                        case client_error::upgrade_rejected:        return "WebSocket upgrade rejected by the server";
                        case client_error::gave_up:                 return "WebSocket reconnect attempts used up";
                    }
                    return "unknown HTTP client error";
                }
        };

    }

    const boost::system::error_category& client_category() noexcept
    {
        static ClientCategory category;
        return category;
    }

    namespace {

        namespace net   = boost::asio;
        namespace ssl   = boost::asio::ssl;
        namespace beast = boost::beast;
        namespace http  = boost::beast::http;
        using tcp       = boost::asio::ip::tcp;
        using Clock     = std::chrono::steady_clock;

        constexpr std::size_t               progress_bytes  = 64 * 1024;
        constexpr std::chrono::milliseconds progress_time   { 100 };
        constexpr std::size_t               write_chunk     = 64 * 1024;
        constexpr std::uint32_t             header_limit    = 64 * 1024;

        bool idempotent(http::verb method)
        {
            switch (method)
            {
                case http::verb::get: case http::verb::head: case http::verb::options:
                case http::verb::put: case http::verb::delete_: case http::verb::trace:
                    return true;
                default:
                    return false;
            }
        }

        using PlainStream = beast::tcp_stream;
        using TlsStream   = beast::ssl_stream<beast::tcp_stream>;

        struct Connection
        {
            std::variant<PlainStream, TlsStream>    stream;
            std::string                             key;
            Clock::time_point                       idle_since;

            Connection(net::any_io_executor executor, std::string k) : stream(std::in_place_type<PlainStream>, executor), key(std::move(k)) {}
            Connection(net::any_io_executor executor, ssl::context& tls, std::string k) : stream(std::in_place_type<TlsStream>, executor, tls), key(std::move(k)) {}

            beast::tcp_stream& tcp()
            {
                return std::visit([](auto& s) -> beast::tcp_stream& { return beast::get_lowest_layer(s); }, stream);
            }

            bool isTls() const { return std::holds_alternative<TlsStream>(stream); }
        };

    }

    namespace detail {

        struct ClientImpl
        {
            HttpClient::Options                                                         options;
            net::io_context                                                             ioc;
            net::executor_work_guard<net::io_context::executor_type>                    work{ ioc.get_executor() };
            ssl::context                                                                tls{ ssl::context::tls_client };
            std::vector<std::thread>                                                    threads;
            std::vector<std::thread::id>                                                thread_ids;

            std::mutex                                                                  mutex;
            std::unordered_map<std::string, std::vector<std::unique_ptr<Connection>>>   idle;
            std::vector<std::weak_ptr<CallState>>                                       calls;
            bool                                                                        closed = false;

            explicit ClientImpl(HttpClient::Options o) : options(std::move(o))
            {
                detail::configureClientTls(tls, options, "HttpClient");
            }

            void start()
            {
                for (std::size_t i = 0; i < std::max<std::size_t>(1, options.threads); ++i)
                    threads.emplace_back([this] { ioc.run(); });
                for (auto& t : threads)
                    thread_ids.push_back(t.get_id());
            }

            bool onClientThread() const
            {
                return std::find(thread_ids.begin(), thread_ids.end(), std::this_thread::get_id()) != thread_ids.end();
            }

            std::unique_ptr<Connection> takeIdle(const std::string& key)
            {
                std::vector<std::unique_ptr<Connection>> expired;
                std::unique_ptr<Connection> found;

                {
                    std::lock_guard lock(mutex);
                    auto it = idle.find(key);
                    if (it == idle.end())
                        return nullptr;

                    auto& list = it->second;
                    auto now = Clock::now();
                    while (!list.empty())
                    {
                        auto conn = std::move(list.back());
                        list.pop_back();
                        if (now - conn->idle_since < options.idle_timeout)
                        {
                            found = std::move(conn);
                            break;
                        }
                        expired.push_back(std::move(conn));
                    }
                }
                // expired connections are closed here, outside the lock
                return found;
            }

            void putIdle(std::unique_ptr<Connection> conn)
            {
                std::unique_ptr<Connection> dropped;

                {
                    std::lock_guard lock(mutex);
                    auto& list = idle[conn->key];
                    if (closed || list.size() >= options.max_idle_per_host)
                    {
                        dropped = std::move(conn);
                    }
                    else
                    {
                        conn->idle_since = Clock::now();
                        list.push_back(std::move(conn));
                    }
                }
            }

            std::size_t idleCount()
            {
                std::lock_guard lock(mutex);
                std::size_t n = 0;
                for (auto& [key, list] : idle)
                    n += list.size();
                return n;
            }
        };

        struct CallState : std::enable_shared_from_this<CallState>
        {
            std::shared_ptr<ClientImpl>                             client;
            net::strand<net::io_context::executor_type>             strand;
            HttpClient::Request                                     request;
            HttpClient::Callbacks                                   callbacks;

            detail::Target                                          target;
            http::request<http::string_body>                        req;
            std::optional<http::serializer<true, http::string_body>> serializer;
            std::optional<http::response_parser<http::string_body>> parser;
            beast::flat_buffer                                      buffer;
            tcp::resolver                                           resolver;
            net::steady_timer                                       deadline;
            std::unique_ptr<Connection>                             conn;

            bool                                                    reused = false;
            bool                                                    retried = false;
            bool                                                    finished = false;
            std::optional<client_error>                             aborted;    // timeout, cancelled or closed

            std::promise<HttpClient::Result>                        promise;
            std::shared_future<HttpClient::Result>                  future = promise.get_future().share();

            // Progress
            std::size_t                                             up_done = 0;
            std::size_t                                             down_done = 0;
            std::optional<std::size_t>                              down_total;
            std::size_t                                             reported_bytes = 0;
            Clock::time_point                                       reported_at;

            CallState(std::shared_ptr<ClientImpl> c, HttpClient::Request r, HttpClient::Callbacks cb)
                : client(std::move(c)), strand(net::make_strand(client->ioc)), request(std::move(r)), callbacks(std::move(cb)),
                  resolver(strand), deadline(strand) {}

            template<typename F>
            auto bind(F&& f)
            {
                return net::bind_executor(strand, std::forward<F>(f));
            }

            // ---- lifecycle

            void start()
            {
                net::dispatch(strand, [self = shared_from_this()] { self->begin(); });
            }

            void begin()
            {
                auto parsed = detail::parseUrl(request.url, detail::UrlKind::http);
                if (!parsed)
                    return finish(parsed.error());
                target = std::move(*parsed);

                req = http::request<http::string_body>{ request.method, target.target, 11 };
                req.set(http::field::host, target.host_header);
                req.set(http::field::user_agent, client->options.user_agent);
                for (const auto& [name, value] : request.headers)
                    req.set(name, value);
                req.body() = std::move(request.body);
                req.prepare_payload();

                deadline.expires_after(request.timeout.value_or(client->options.request_timeout));
                deadline.async_wait(bind([self = shared_from_this()](boost::system::error_code ec) {
                    if (!ec)
                        self->abort(client_error::timeout);
                }));

                connect();
            }

            void abort(client_error why)
            {
                if (finished || aborted)
                    return;

                // The pending operation completes with an error, handled where it lands
                aborted = why;
                resolver.cancel();
                if (conn)
                {
                    boost::system::error_code ignored;
                    conn->tcp().socket().close(ignored);
                }
            }

            void finish(HttpClient::Result result)
            {
                if (finished)
                    return;
                finished = true;

                deadline.cancel();
                conn.reset();       // not given back to the pool: closed

                promise.set_value(result);

                // Released after the call, so a callback capturing the Call handle doesn't keep us alive
                auto onComplete = std::move(callbacks.onComplete);
                callbacks.onProgress = nullptr;
                if (onComplete)
                    deliver([onComplete = std::move(onComplete), result = std::move(result)]() mutable { onComplete(std::move(result)); });
            }

            template<typename F>
            void deliver(F&& f)
            {
                if (callbacks.executor)
                {
                    net::post(callbacks.executor, std::forward<F>(f));
                    return;
                }

                // An exception escaping here would stop the client's io_context
                try { f(); } catch (...) {}
            }

            void fail(boost::system::error_code ec, bool during_write)
            {
                if (aborted)
                    return finish(make_error_code(*aborted));

                if (ec == http::error::body_limit || ec == http::error::buffer_overflow)
                    return finish(make_error_code(client_error::response_too_large));

                if (ec == beast::error::timeout)
                    return finish(make_error_code(client_error::timeout));

                // A kept-alive connection the server closed in the meantime: try once more on a new one
                bool nothing_received = !parser || !parser->got_some();
                if (reused && !retried && nothing_received && (idempotent(req.method()) || during_write))
                {
                    retried = true;
                    reused = false;
                    conn.reset();
                    up_done = 0;
                    reported_bytes = 0;
                    parser.reset();
                    buffer.consume(buffer.size());
                    return resolve();
                }

                finish(ec);
            }

            // ---- connection

            void connect()
            {
                if (auto idle = client->takeIdle(target.key))
                {
                    conn = std::move(idle);
                    reused = true;
                    return writeHeader();
                }
                resolve();
            }

            void resolve()
            {
                resolver.async_resolve(target.host, target.port, bind([self = shared_from_this()](boost::system::error_code ec, tcp::resolver::results_type results) {
                    if (ec)
                        return self->fail(ec, false);
                    self->onResolve(results);
                }));
            }

            void onResolve(const tcp::resolver::results_type& results)
            {
                if (aborted)
                    return finish(make_error_code(*aborted));

                conn = target.tls ? std::make_unique<Connection>(strand, client->tls, target.key)
                                  : std::make_unique<Connection>(strand, target.key);

                conn->tcp().expires_after(client->options.connect_timeout);
                conn->tcp().async_connect(results, bind([self = shared_from_this()](boost::system::error_code ec, const tcp::endpoint&) {
                    if (ec)
                        return self->fail(ec, false);
                    self->onConnect();
                }));
            }

            void onConnect()
            {
                if (!conn->isTls())
                    return writeHeader();

                auto& stream = std::get<TlsStream>(conn->stream);

                // SNI: which certificate the server should present
                if (!SSL_set_tlsext_host_name(stream.native_handle(), target.host.c_str()))
                    return finish(boost::system::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()));

                // Host name check: the certificate must be for this host
                if (client->options.verify_peer)
                    stream.set_verify_callback(ssl::host_name_verification(target.host));

                stream.async_handshake(ssl::stream_base::client, bind([self = shared_from_this()](boost::system::error_code ec) {
                    if (ec)
                        return self->fail(ec, false);
                    self->writeHeader();
                }));
            }

            // ---- request

            template<typename F>
            void withStream(F&& f)
            {
                std::visit(std::forward<F>(f), conn->stream);
            }

            void writeHeader()
            {
                // The deadline timer bounds the request from here
                conn->tcp().expires_never();

                serializer.emplace(req);
                serializer->limit(write_chunk);

                withStream([this](auto& stream) {
                    http::async_write_header(stream, *serializer, bind([self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                        if (ec)
                            return self->fail(ec, true);
                        self->writeBody();
                    }));
                });
            }

            void writeBody()
            {
                if (serializer->is_done())
                {
                    if (!req.body().empty())
                        progress(HttpClient::Progress::Direction::upload, up_done, req.body().size(), true);
                    return readHeader();
                }

                withStream([this](auto& stream) {
                    http::async_write_some(stream, *serializer, bind([self = shared_from_this()](boost::system::error_code ec, std::size_t n) {
                        if (ec)
                            return self->fail(ec, true);
                        self->up_done += n;
                        // The serializer may finish with an empty write: only a body has upload progress
                        if (!self->req.body().empty())
                            self->progress(HttpClient::Progress::Direction::upload, self->up_done, self->req.body().size(), false);
                        self->writeBody();
                    }));
                });
            }

            // ---- response

            void readHeader()
            {
                reported_bytes = 0;
                parser.emplace();
                parser->header_limit(header_limit);
                parser->body_limit(client->options.max_response_size);
                if (req.method() == http::verb::head)
                    parser->skip(true);

                withStream([this](auto& stream) {
                    http::async_read_header(stream, buffer, *parser, bind([self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                        if (ec)
                            return self->fail(ec, false);
                        // A HEAD response announces the size of a body it doesn't send
                        if (auto length = self->parser->content_length(); length && self->req.method() != http::verb::head)
                            self->down_total = static_cast<std::size_t>(*length);
                        self->readBody();
                    }));
                });
            }

            void readBody()
            {
                if (parser->is_done())
                {
                    down_done = parser->get().body().size();
                    progress(HttpClient::Progress::Direction::download, down_done, down_total.value_or(down_done), true);
                    return succeed();
                }

                withStream([this](auto& stream) {
                    http::async_read_some(stream, buffer, *parser, bind([self = shared_from_this()](boost::system::error_code ec, std::size_t) {
                        if (ec)
                            return self->fail(ec, false);
                        self->down_done = self->parser->get().body().size();
                        self->progress(HttpClient::Progress::Direction::download, self->down_done, self->down_total, false);
                        self->readBody();
                    }));
                });
            }

            void succeed()
            {
                bool keep_alive = parser->get().keep_alive() && !aborted;
                auto response = parser->release();

                if (keep_alive)
                    client->putIdle(std::move(conn));

                finish(HttpClient::Response(std::move(response)));
            }

            // ---- progress

            void progress(HttpClient::Progress::Direction direction, std::size_t done, std::optional<std::size_t> total, bool last)
            {
                if (!callbacks.onProgress)
                    return;

                auto now = Clock::now();
                if (!last && done - reported_bytes < progress_bytes && now - reported_at < progress_time)
                    return;

                reported_bytes = done;
                reported_at = now;

                deliver([onProgress = callbacks.onProgress, p = HttpClient::Progress{ direction, done, total }] { onProgress(p); });
            }
        };

    }//namespace detail

    // ---- HttpClient

    HttpClient::HttpClient() : HttpClient(Options{}) {}

    HttpClient::HttpClient(Options options)
        : impl_(std::make_shared<detail::ClientImpl>(std::move(options)))
    {
        impl_->start();
    }

    HttpClient::~HttpClient()
    {
        std::vector<std::shared_ptr<detail::CallState>> in_flight;
        std::unordered_map<std::string, std::vector<std::unique_ptr<Connection>>> idle;

        {
            std::lock_guard lock(impl_->mutex);
            impl_->closed = true;
            idle.swap(impl_->idle);
            for (auto& weak : impl_->calls)
                if (auto call = weak.lock())
                    in_flight.push_back(std::move(call));
            impl_->calls.clear();
        }

        for (auto& call : in_flight)
            net::post(call->strand, [call] { call->abort(client_error::closed); });

        idle.clear();
        impl_->work.reset();

        for (auto& thread : impl_->threads)
        {
            // Destroyed from one of its own callbacks: can't join itself
            if (thread.get_id() == std::this_thread::get_id())
                thread.detach();
            else
                thread.join();
        }
    }

    HttpClient::Call HttpClient::send(Request request, Callbacks callbacks)
    {
        auto state = std::make_shared<detail::CallState>(impl_, std::move(request), std::move(callbacks));

        bool closed;
        {
            std::lock_guard lock(impl_->mutex);
            closed = impl_->closed;
            if (!closed)
            {
                auto& calls = impl_->calls;
                calls.erase(std::remove_if(calls.begin(), calls.end(), [](auto& w) { return w.expired(); }), calls.end());
                calls.push_back(state);
            }
        }

        if (closed)
            net::dispatch(state->strand, [state] { state->finish(make_error_code(client_error::closed)); });
        else
            state->start();

        return Call(state);
    }

    std::size_t HttpClient::idleConnections() const
    {
        return impl_->idleCount();
    }

    // ---- Call

    void HttpClient::Call::cancel()
    {
        if (!state_)
            return;
        net::post(state_->strand, [state = state_] { state->abort(client_error::cancelled); });
    }

    HttpClient::Result HttpClient::Call::wait()
    {
        if (!state_)
            return make_error_code(client_error::invalid_url);

        if (state_->client->onClientThread() && !done())
            return make_error_code(client_error::wait_on_client_thread);

        return state_->future.get();
    }

    bool HttpClient::Call::done() const
    {
        return state_ && state_->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

}//namespace gtvr
