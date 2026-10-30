#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <cppgate/cppgate.h>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gtvr {

    // Errors of the client itself; network, DNS and TLS errors keep their Asio / OpenSSL error codes
    enum class client_error
    {
        timeout = 1,                // request_timeout (or connect_timeout) elapsed
        cancelled,                  // Call::cancel()
        response_too_large,         // over Options::max_response_size
        invalid_url,
        unsupported_scheme,         // only http:// and https://
        closed,                     // the client was destroyed
        wait_on_client_thread,      // Call::wait() from a client callback would deadlock
    };

    CPPGATE_API const boost::system::error_category& client_category() noexcept;

    inline boost::system::error_code make_error_code(client_error e) noexcept
    {
        return { static_cast<int>(e), client_category() };
    }

}//namespace gtvr

template<>
struct boost::system::is_error_code_enum<gtvr::client_error> : std::true_type {};

namespace gtvr {

    namespace detail {
        struct ClientImpl;
        struct CallState;
    }

    /*
        Asynchronous HTTP/1.1 client, HTTP and HTTPS.

            gtvr::HttpClient client;

            client.send({ .method = http::verb::post, .url = "https://api.example.com/items", .body = json },
            {
                .onProgress = [](const gtvr::HttpClient::Progress& p) { ... },
                .onComplete = [](gtvr::HttpClient::Result r) {
                    if (!r) { r.error(); return; }      // network, DNS, TLS, timeout...
                    r->status(); r->body();             // 4xx and 5xx are responses, not errors
                },
            });

            auto r = client.send({ .url = "https://example.com" }).wait();     // or wait for it

        - Runs on its own threads (Options::threads): callbacks run there, unless Callbacks::executor is set.
        - onComplete is called exactly once: success, error, timeout, cancel or client destroyed.
        - HTTPS is verified against the Mozilla CA store bundled from curl (plus Options::extra_ca_*).
        - Connections are kept alive and reused per scheme, host and port. A request that fails on a
          reused connection the server had closed is retried once on a new one.
    */
    class CPPGATE_API HttpClient
    {
        public:
            struct Options
            {
                // Threads running the client and its callbacks
                std::size_t                 threads                     = 1;

                // DNS + TCP connect + TLS handshake of a new connection
                std::chrono::milliseconds   connect_timeout             { 10000 };

                // Whole request, from send() to the last byte of the response (Request::timeout overrides)
                std::chrono::milliseconds   request_timeout             { 30000 };

                // Response body; over it the request fails with client_error::response_too_large
                std::size_t                 max_response_size           = 64 * 1024 * 1024;

                // Idle keep-alive connections kept per host, and for how long
                std::size_t                 max_idle_per_host           = 8;
                std::chrono::milliseconds   idle_timeout                { 30000 };

                // HTTPS: verify the server's certificate and host name. Only turn off for local development.
                bool                        verify_peer                 = true;

                // Where trusted CAs come from; any combination. With verify_peer and none of them,
                // the constructor throws.

                // The Mozilla CA store bundled with cppgate (see tools/update_ca_bundle.py)
                bool                        use_bundled_cas             = true;

                // The operating system's store: $SSL_CERT_FILE / $SSL_CERT_DIR if set, and the first
                // well-known bundle found (/etc/ssl/certs/ca-certificates.crt on Debian/Ubuntu,
                // /etc/pki/tls/certs/ca-bundle.crt on Fedora/RHEL, /etc/ssl/cert.pem on macOS...).
                // Windows has no such file: there only the environment variables count.
                bool                        use_system_cas              = false;

                // More trusted CAs: PEM files (one or many certificates each), directories in OpenSSL's
                // hashed format (<subject hash>.0, as made by c_rehash / openssl rehash), or PEM text
                std::vector<std::string>    extra_ca_files;
                std::vector<std::string>    extra_ca_dirs;
                std::vector<std::string>    extra_ca_pem;

                std::string                 user_agent                  = "cppgate";
            };

            struct Request
            {
                boost::beast::http::verb                            method  = boost::beast::http::verb::get;
                std::string                                         url;
                std::vector<std::pair<std::string, std::string>>    headers;
                std::string                                         body;
                std::optional<std::chrono::milliseconds>            timeout;
            };

            class Response
            {
                private:
                    boost::beast::http::response<boost::beast::http::string_body> message_;

                public:
                    explicit Response(boost::beast::http::response<boost::beast::http::string_body> message) : message_(std::move(message)) {}

                    unsigned status() const noexcept            { return message_.result_int(); }

                    std::string_view header(std::string_view name) const
                    {
                        auto it = message_.find(name);
                        return it == message_.end() ? std::string_view{} : std::string_view(it->value());
                    }

                    const std::string& body() const noexcept    { return message_.body(); }
                    std::string& body() noexcept                { return message_.body(); }

                    // Everything else: all headers, version...
                    const boost::beast::http::response<boost::beast::http::string_body>& message() const noexcept { return message_; }

                    // "HTTP 200, 1234 bytes": for logs (and printing a Result)
                    friend std::ostream& operator<<(std::ostream& os, const Response& r)
                    {
                        return os << "HTTP " << r.status() << ", " << r.body().size() << " bytes";
                    }
            };

            using Result = boost::system::result<Response>;

            struct Progress
            {
                enum class Direction { upload, download };

                Direction                   direction;
                std::size_t                 transferred;    // body bytes
                std::optional<std::size_t>  total;          // unknown for a chunked response without Content-Length
            };

            struct Callbacks
            {
                // Upload (while the request body is sent, only if there is one), then download. At most
                // every 64 KB or 100 ms, and a last event per direction when it is complete. Upload counts
                // bytes handed to the network, not bytes the server has already received.
                std::function<void(const Progress&)>    onProgress;

                // Exactly once
                std::function<void(Result)>             onComplete;

                // Where the callbacks run (e.g. a WebSocket connection's strand). Default: the client's threads.
                boost::asio::any_io_executor            executor;
            };

            // A request in flight. Dropping the handle doesn't cancel it.
            class CPPGATE_API Call
            {
                private:
                    std::shared_ptr<detail::CallState> state_;

                    friend HttpClient;

                    explicit Call(std::shared_ptr<detail::CallState> state) : state_(std::move(state)) {}

                public:
                    // From any thread; onComplete gets client_error::cancelled (unless already complete)
                    void cancel();

                    // Blocks until complete. From a client callback it returns client_error::wait_on_client_thread
                    // instead of deadlocking.
                    Result wait();

                    bool done() const;
            };

        private:
            std::shared_ptr<detail::ClientImpl> impl_;

        public:
            HttpClient();

            // Throws boost::system::system_error if a CA file / directory in the options can't be loaded,
            // std::invalid_argument if verify_peer is on and no CA source is configured or found
            explicit HttpClient(Options options);

            // Cancels the requests in flight (onComplete gets client_error::closed) and stops the threads.
            // Don't destroy the client from one of its own callbacks: it can't join its own thread.
            ~HttpClient();

            HttpClient(const HttpClient&) = delete;
            HttpClient& operator=(const HttpClient&) = delete;

            Call send(Request request, Callbacks callbacks = {});

            // Idle keep-alive connections, all hosts
            std::size_t idleConnections() const;
    };

}//namespace gtvr
