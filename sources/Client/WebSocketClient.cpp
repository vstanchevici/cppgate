#include <cppgate/Client/WebSocketClient.h>

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
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <variant>

namespace gtvr {

    namespace {

        namespace net       = boost::asio;
        namespace ssl       = boost::asio::ssl;
        namespace beast     = boost::beast;
        namespace http      = boost::beast::http;
        namespace websocket = boost::beast::websocket;
        using tcp           = boost::asio::ip::tcp;

        // After the close frames, Beast's TLS teardown waits for the server's close_notify. Some servers
        // never send it: they wait for the client to close TCP. The WebSocket close is already complete
        // at that point, so the TLS goodbye only gets this long.
        constexpr std::chrono::milliseconds teardown_grace { 500 };

        // Our own type, so the teardown below is picked by Beast (found by ADL, better match than its own)
        struct TlsLayer : beast::ssl_stream<beast::tcp_stream>
        {
            using beast::ssl_stream<beast::tcp_stream>::ssl_stream;
        };

        template<class TeardownHandler>
        void async_teardown(beast::role_type, TlsLayer& stream, TeardownHandler&& handler)
        {
            beast::get_lowest_layer(stream).expires_after(teardown_grace);
            stream.async_shutdown([&stream, handler = std::forward<TeardownHandler>(handler)](boost::system::error_code) mutable {
                // Answered, truncated or timed out: the WebSocket close is done either way
                boost::system::error_code ignored;
                beast::get_lowest_layer(stream).socket().close(ignored);
                handler(boost::system::error_code{});
            });
        }

        void teardown(beast::role_type, TlsLayer& stream, boost::system::error_code& ec)
        {
            beast::get_lowest_layer(stream).expires_after(teardown_grace);
            boost::system::error_code ignored;
            stream.shutdown(ignored);
            beast::get_lowest_layer(stream).socket().close(ignored);
            ec = {};
        }

        using PlainWs = websocket::stream<beast::tcp_stream>;
        using TlsWs   = websocket::stream<TlsLayer>;
        using Stream  = std::variant<PlainWs, TlsWs>;

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

        // Delay before retry number `attempt` (1, 2, ...): doubling, capped, +-20% jitter
        std::chrono::milliseconds retryDelay(const WebSocketClient::Reconnect& r, unsigned attempt)
        {
            double delay = static_cast<double>(r.initial_delay.count());
            for (unsigned i = 1; i < attempt && delay < static_cast<double>(r.max_delay.count()); ++i)
                delay *= 2;
            delay = std::min(delay, static_cast<double>(r.max_delay.count()));

            thread_local std::mt19937 rng{ std::random_device{}() };
            std::uniform_real_distribution<double> jitter(0.8, 1.2);
            return std::chrono::milliseconds(static_cast<long long>(delay * jitter(rng)));
        }

    }

    namespace detail {

        struct WsClientImpl
        {
            WebSocketClient::Options                                    options;
            net::io_context                                             ioc;
            net::executor_work_guard<net::io_context::executor_type>    work{ ioc.get_executor() };
            ssl::context                                                tls{ ssl::context::tls_client };
            std::vector<std::thread>                                    threads;

            std::mutex                                                  mutex;
            std::vector<std::weak_ptr<WsConnection>>                    connections;
            bool                                                        closed = false;

            explicit WsClientImpl(WebSocketClient::Options o) : options(std::move(o))
            {
                configureClientTls(tls, options, "WebSocketClient");

                for (std::size_t i = 0; i < std::max<std::size_t>(1, options.threads); ++i)
                    threads.emplace_back([this] { ioc.run(); });
            }

            bool isClosed()
            {
                std::lock_guard lock(mutex);
                return closed;
            }
        };

        struct WsConnection : WebSocketClient::Connection, std::enable_shared_from_this<WsConnection>
        {
            using CloseCode = WebSocketClient::CloseCode;

            enum class State { idle, connecting, open, closing, waiting, stopped };

            std::shared_ptr<WsClientImpl>                               client;
            net::strand<net::io_context::executor_type>                 strand;
            WebSocketClient::Request                                    request;
            WebSocketClient::Callbacks                                  callbacks;
            Target                                                      target;

            tcp::resolver                                               resolver;
            net::steady_timer                                           retry_timer;

            // One stream per attempt. Pending operations hold their stream, so replacing it is safe;
            // `generation` tells their completions apart from the current attempt's.
            std::shared_ptr<Stream>                                     ws;
            std::atomic<unsigned>                                       generation{ 0 };     // written on the strand, read by send() too
            beast::flat_buffer                                          read_buffer;
            websocket::response_type                                    upgrade_response;
            std::deque<std::pair<std::shared_ptr<const std::string>, bool>> write_queue;

            // Strand only
            State                                                       state = State::idle;
            bool                                                        stop_requested = false;
            unsigned                                                    failures = 0;      // attempts in a row without success

            // Our own close: Beast cancels the pending read when the close handshake completes,
            // so the outcome comes from the close operation; whichever of the two finishes last reports
            CloseCode                                                   requested_code = CloseCode::normal;
            bool                                                        close_done = false;
            boost::system::error_code                                   close_result;
            bool                                                        read_done = false;

            // Any thread
            std::atomic<bool>                                           connected{ false };
            std::atomic<std::size_t>                                    pending{ 0 };
            std::atomic<unsigned>                                       upgrade_status{ 0 };
            mutable std::mutex                                          protocol_mutex;
            std::string                                                 protocol_;

            WsConnection(std::shared_ptr<WsClientImpl> c, WebSocketClient::Request r, WebSocketClient::Callbacks cb)
                : client(std::move(c)), strand(net::make_strand(client->ioc)), request(std::move(r)), callbacks(std::move(cb)),
                  resolver(strand), retry_timer(strand) {}

            // ---- public API, any thread

            bool sendText(std::string data) override
            {
                return send(std::make_shared<const std::string>(std::move(data)), true);
            }

            bool sendText(std::shared_ptr<const std::string> data) override
            {
                return send(std::move(data), true);
            }

            bool sendBinary(std::vector<std::uint8_t> data) override
            {
                return send(std::make_shared<const std::string>(data.begin(), data.end()), false);
            }

            void close(CloseCode code) override
            {
                net::post(strand, [self = shared_from_this(), code] { self->doClose(code); });
            }

            bool isConnected() const override { return connected; }

            std::string protocol() const override
            {
                std::lock_guard lock(protocol_mutex);
                return protocol_;
            }

            unsigned upgradeStatus() const override { return upgrade_status; }

            std::size_t pendingMessages() const override { return pending; }

            // ---- events

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

            void emitOpen()
            {
                if (callbacks.onOpen)
                    deliver([self = shared_from_this(), fn = callbacks.onOpen] { fn(*self); });
            }

            void emitClose(CloseCode code)
            {
                if (callbacks.onClose)
                    deliver([self = shared_from_this(), fn = callbacks.onClose, code] { fn(*self, code); });
            }

            void emitError(boost::system::error_code ec)
            {
                if (callbacks.onError)
                    deliver([self = shared_from_this(), fn = callbacks.onError, ec] { fn(*self, ec); });
            }

            void emitReconnecting(unsigned attempt, std::chrono::milliseconds delay)
            {
                if (callbacks.onReconnecting)
                    deliver([self = shared_from_this(), fn = callbacks.onReconnecting, attempt, delay] { fn(*self, attempt, delay); });
            }

            void emitMessage(std::string_view data, bool is_text)
            {
                if (!callbacks.onMessage)
                    return;

                if (!callbacks.executor)
                {
                    // The view is valid during the call: no copy
                    try { callbacks.onMessage(*this, WebSocketMessage(data, is_text)); } catch (...) {}
                    return;
                }

                // Runs later on another executor: it needs its own copy
                net::post(callbacks.executor, [self = shared_from_this(), fn = callbacks.onMessage, copy = std::string(data), is_text] {
                    fn(*self, WebSocketMessage(copy, is_text));
                });
            }

            // ---- lifecycle, strand

            void start()
            {
                net::dispatch(strand, [self = shared_from_this()] {
                    auto parsed = parseUrl(self->request.url, UrlKind::websocket);
                    if (!parsed)
                    {
                        self->emitError(parsed.error());
                        return self->stop();
                    }
                    self->target = std::move(*parsed);
                    self->attempt();
                });
            }

            void stop()
            {
                state = State::stopped;
                connected = false;

                // Nothing more will be called: release the user's callbacks
                auto executor = callbacks.executor;
                callbacks = {};
                callbacks.executor = executor;
            }

            bool stale(unsigned gen) const
            {
                return gen != generation || state == State::stopped;
            }

            template<typename F>
            auto bind(F&& f)
            {
                return net::bind_executor(strand, std::forward<F>(f));
            }

            void attempt()
            {
                if (stop_requested || client->isClosed())
                    return stop();

                state = State::connecting;
                unsigned gen = ++generation;

                resolver.async_resolve(target.host, target.port, bind([self = shared_from_this(), gen](boost::system::error_code ec, tcp::resolver::results_type results) {
                    if (self->stale(gen))
                        return;
                    if (ec)
                        return self->attemptFailed(ec);
                    self->onResolve(gen, results);
                }));
            }

            void onResolve(unsigned gen, const tcp::resolver::results_type& results)
            {
                ws = target.tls ? std::make_shared<Stream>(std::in_place_type<TlsWs>, strand, client->tls)
                                : std::make_shared<Stream>(std::in_place_type<PlainWs>, strand);

                auto& tcp_stream = std::visit([](auto& s) -> beast::tcp_stream& { return beast::get_lowest_layer(s); }, *ws);
                tcp_stream.expires_after(client->options.connect_timeout);
                tcp_stream.async_connect(results, bind([self = shared_from_this(), gen, s = ws](boost::system::error_code ec, const tcp::endpoint&) {
                    if (self->stale(gen))
                        return;
                    if (ec)
                        return self->attemptFailed(ec);
                    self->onConnect(gen);
                }));
            }

            void onConnect(unsigned gen)
            {
                if (!target.tls)
                    return upgrade(gen);

                auto& tls_stream = std::get<TlsWs>(*ws).next_layer();

                // SNI, and the certificate must be for this host
                if (!SSL_set_tlsext_host_name(tls_stream.native_handle(), target.host.c_str()))
                    return attemptFailed(boost::system::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()));
                if (client->options.verify_peer)
                    tls_stream.set_verify_callback(ssl::host_name_verification(target.host));

                tls_stream.async_handshake(ssl::stream_base::client, bind([self = shared_from_this(), gen, s = ws](boost::system::error_code ec) {
                    if (self->stale(gen))
                        return;
                    if (ec)
                        return self->attemptFailed(ec);
                    self->upgrade(gen);
                }));
            }

            void upgrade(unsigned gen)
            {
                const auto& options = client->options;

                std::visit([&](auto& s) {
                    // The WebSocket timeouts below take over from the TCP one
                    beast::get_lowest_layer(s).expires_never();

                    websocket::stream_base::timeout timeouts;
                    timeouts.handshake_timeout = options.connect_timeout;
                    timeouts.idle_timeout = options.idle_timeout.count() > 0
                        ? websocket::stream_base::duration(options.idle_timeout)
                        : websocket::stream_base::none();
                    timeouts.keep_alive_pings = options.keep_alive_pings;
                    s.set_option(timeouts);

                    s.read_message_max(options.max_message_size);

                    websocket::permessage_deflate deflate;
                    deflate.client_enable = options.compression;
                    s.set_option(deflate);

                    s.set_option(websocket::stream_base::decorator(
                        [ua = options.user_agent, headers = request.headers, protocols = join(request.subprotocols)](websocket::request_type& req) {
                            req.set(http::field::user_agent, ua);
                            for (const auto& [name, value] : headers)
                                req.set(name, value);
                            if (!protocols.empty())
                                req.set(http::field::sec_websocket_protocol, protocols);
                        }));

                    upgrade_response = {};
                    s.async_handshake(upgrade_response, target.host_header, target.target,
                        bind([self = shared_from_this(), gen, keep = ws](boost::system::error_code ec) {
                            if (self->stale(gen))
                                return;
                            self->onUpgrade(gen, ec);
                        }));
                }, *ws);
            }

            void onUpgrade(unsigned gen, boost::system::error_code ec)
            {
                if (ec == websocket::error::upgrade_declined)
                {
                    // The server answered with an HTTP status: retrying wouldn't change its mind
                    upgrade_status = upgrade_response.result_int();
                    if (!stop_requested)
                        emitError(make_error_code(client_error::upgrade_rejected));
                    return stop();
                }

                if (ec)
                    return attemptFailed(ec);

                {
                    std::lock_guard lock(protocol_mutex);
                    protocol_ = std::string(upgrade_response[http::field::sec_websocket_protocol]);
                }

                upgrade_status = 0;
                failures = 0;

                if (stop_requested)
                {
                    // close() came during the handshake: close now that it's open
                    state = State::open;
                    connected = true;
                    emitOpen();
                    read(gen);
                    return closeOpen(CloseCode::normal);
                }

                state = State::open;
                connected = true;
                emitOpen();
                read(gen);
            }

            void attemptFailed(boost::system::error_code ec)
            {
                if (stop_requested)
                    return stop();

                emitError(ec);
                scheduleRetry();
            }

            void scheduleRetry()
            {
                if (!request.reconnect.enabled || stop_requested || client->isClosed())
                    return stop();

                ++failures;
                if (request.reconnect.max_attempts > 0 && failures > request.reconnect.max_attempts)
                {
                    emitError(make_error_code(client_error::gave_up));
                    return stop();
                }

                auto delay = retryDelay(request.reconnect, failures);
                state = State::waiting;
                emitReconnecting(failures, delay);

                retry_timer.expires_after(delay);
                retry_timer.async_wait(bind([self = shared_from_this()](boost::system::error_code ec) {
                    if (ec || self->state != State::waiting)
                        return;
                    self->attempt();
                }));
            }

            // ---- open connection

            void read(unsigned gen)
            {
                std::visit([&](auto& s) {
                    s.async_read(read_buffer, bind([self = shared_from_this(), gen, keep = ws](boost::system::error_code ec, std::size_t) {
                        if (self->stale(gen))
                            return;
                        self->onRead(gen, ec);
                    }));
                }, *ws);
            }

            CloseCode peerCode() const
            {
                auto code = std::visit([](auto& s) { return CloseCode(s.reason().code); }, *ws);
                return code == CloseCode::none ? CloseCode::no_status : code;
            }

            void onRead(unsigned gen, boost::system::error_code ec)
            {
                if (ec)
                    return ended(ec);

                bool is_text = std::visit([](auto& s) { return s.got_text(); }, *ws);
                emitMessage(std::string_view(static_cast<const char*>(read_buffer.data().data()), read_buffer.size()), is_text);
                read_buffer.consume(read_buffer.size());

                // Also during our own close(): Beast wants reads to continue until the close completes
                read(gen);
            }

            void ended(boost::system::error_code ec)
            {
                if (state == State::closing && ec == net::error::operation_aborted)
                {
                    read_done = true;
                    if (close_done)
                        finishOwnClose();
                    return;
                }

                CloseCode code = CloseCode::abnormal;

                if (ec == websocket::error::closed)
                    code = peerCode();
                else if (!stop_requested && ec != net::error::operation_aborted)
                    emitError(ec);

                closed(code);
            }

            // Our close() completed: the server's echoed code, or abnormal if the handshake failed / timed out
            void finishOwnClose()
            {
                CloseCode code = CloseCode::abnormal;
                if (!close_result)
                {
                    auto echoed = std::visit([](auto& s) { return CloseCode(s.reason().code); }, *ws);
                    code = echoed == CloseCode::none ? requested_code : echoed;
                }
                closed(code);
            }

            void closed(CloseCode code)
            {
                connected = false;
                pending -= write_queue.size();
                write_queue.clear();
                read_buffer.consume(read_buffer.size());

                emitClose(code);

                if (stop_requested || client->isClosed())
                    return stop();

                scheduleRetry();
            }

            bool send(std::shared_ptr<const std::string> message, bool is_text)
            {
                if (!connected)
                    return false;

                // Reserve a place, never above the limit even with many senders
                auto current = pending.load();
                do {
                    if (current >= client->options.send_queue_limit)
                        return false;
                } while (!pending.compare_exchange_weak(current, current + 1));

                net::post(strand, [self = shared_from_this(), message = std::move(message), is_text, gen = generation.load()]() mutable {
                    if (self->state != State::open || gen != self->generation)
                    {
                        --self->pending;
                        return;
                    }

                    self->write_queue.emplace_back(std::move(message), is_text);
                    if (self->write_queue.size() == 1)
                        self->write(self->generation);
                });
                return true;
            }

            void write(unsigned gen)
            {
                auto message = write_queue.front().first;
                bool is_text = write_queue.front().second;

                std::visit([&](auto& s) {
                    s.text(is_text);
                    s.async_write(net::buffer(*message), bind([self = shared_from_this(), gen, keep = ws, hold = message](boost::system::error_code ec, std::size_t) {
                        if (self->stale(gen) || self->write_queue.empty())
                            return;
                        if (ec)
                            return;     // the read side reports it and ends the connection
                        self->write_queue.pop_front();
                        --self->pending;
                        if (!self->write_queue.empty())
                            self->write(gen);
                    }));
                }, *ws);
            }

            // ---- close

            void doClose(CloseCode code)
            {
                if (stop_requested || state == State::stopped)
                    return;
                stop_requested = true;

                switch (state)
                {
                    case State::open:
                        return closeOpen(code);

                    case State::connecting:
                        // The pending step fails and sees stop_requested; the handshake case closes in onUpgrade
                        resolver.cancel();
                        if (ws)
                        {
                            boost::system::error_code ignored;
                            std::visit([&](auto& s) { beast::get_lowest_layer(s).socket().close(ignored); }, *ws);
                        }
                        return;

                    case State::waiting:
                        retry_timer.cancel();
                        return stop();

                    default:
                        return stop();
                }
            }

            void closeOpen(CloseCode code)
            {
                state = State::closing;
                requested_code = code;
                unsigned gen = generation;

                std::visit([&](auto& s) {
                    websocket::stream_base::timeout timeouts;
                    s.get_option(timeouts);
                    timeouts.handshake_timeout = client->options.close_timeout;
                    s.set_option(timeouts);

                    s.async_close(code, bind([self = shared_from_this(), gen, keep = ws](boost::system::error_code ec) {
                        if (self->stale(gen))
                            return;
                        self->close_done = true;
                        self->close_result = ec;
                        if (self->read_done)
                            self->finishOwnClose();
                    }));
                }, *ws);
            }
        };

    }//namespace detail

    // ---- WebSocketClient

    WebSocketClient::WebSocketClient() : WebSocketClient(Options{}) {}

    WebSocketClient::WebSocketClient(Options options)
        : impl_(std::make_shared<detail::WsClientImpl>(std::move(options)))
    {
    }

    WebSocketClient::~WebSocketClient()
    {
        std::vector<std::shared_ptr<detail::WsConnection>> live;

        {
            std::lock_guard lock(impl_->mutex);
            impl_->closed = true;
            for (auto& weak : impl_->connections)
                if (auto c = weak.lock())
                    live.push_back(std::move(c));
            impl_->connections.clear();
        }

        for (auto& connection : live)
            connection->close(CloseCode::going_away);
        live.clear();

        impl_->work.reset();

        for (auto& thread : impl_->threads)
        {
            if (thread.get_id() == std::this_thread::get_id())
                thread.detach();
            else
                thread.join();
        }
    }

    std::shared_ptr<WebSocketClient::Connection> WebSocketClient::connect(Request request, Callbacks callbacks)
    {
        auto connection = std::make_shared<detail::WsConnection>(impl_, std::move(request), std::move(callbacks));

        {
            std::lock_guard lock(impl_->mutex);
            auto& list = impl_->connections;
            list.erase(std::remove_if(list.begin(), list.end(), [](auto& w) { return w.expired(); }), list.end());
            list.push_back(connection);
        }

        connection->start();

        // The handle closes the connection when the last copy goes away; the connection itself lives on
        // until its pending operations finish
        return std::shared_ptr<Connection>(connection.get(), [connection](Connection*) mutable {
            connection->close(CloseCode::normal);
            connection.reset();
        });
    }

}//namespace gtvr
