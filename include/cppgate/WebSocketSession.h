#pragma once

#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <cppgate/Log.h>
#include <cppgate/Router.h>
#include <functional>
#include <deque>
#include "WebSocketSessionInterface.h"

namespace gtvr {

// Define stream types
using ws_plain_t = boost::beast::websocket::stream<boost::beast::tcp_stream>;
using ws_ssl_t  = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;

// Declare the type ws_stream_t
template<bool with_plain, bool with_ssl>
using ws_stream_t = std::conditional_t<
        with_plain && with_ssl,
        std::variant<ws_plain_t, ws_ssl_t>, // Both
        std::conditional_t<
            with_plain,
            std::variant<ws_plain_t>,      // Plain only
            std::variant<ws_ssl_t>         // SSL only
        >
>;

template<bool with_plain, bool with_ssl>
class WebSocketSession: public WebSocketSessionInterface, public std::enable_shared_from_this<WebSocketSession<with_plain, with_ssl>>
{
    static_assert(with_plain || with_ssl, "WebSocketSession must have or plain or ssl stream.");

    using std::enable_shared_from_this<WebSocketSession<with_plain, with_ssl>>::shared_from_this;

    struct ws_message {
        // We use const to ensure thread-safety across multiple sessions
        std::shared_ptr<const std::string> data;
        bool is_text;

        ws_message(std::shared_ptr<const std::string> d, bool t)
            : data(std::move(d)), is_text(t) {}
    };
    
    private:
        static constexpr std::size_t                                    queue_limit = 32; // max write
        std::deque<ws_message>                                          write_queue_;

        boost::beast::flat_buffer  buffer_;
        boost::beast::http::request<boost::beast::http::string_body>    request_;
        ws_stream_t<with_plain, with_ssl> stream_;
    
        // Start the asynchronous operation
        template<class Body, class Allocator>
        void do_accept(boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>> req, std::shared_ptr<gtvr::router::Router> router)
        {
            request_ = req;

            /*
             std::visit([this](auto& ws) {
                 ws.control_callback(
                     [this](beast::websocket::frame_type kind, beast::string_view payload) {
                         if(kind == beast::websocket::frame_type::pong) {
                             // The client is alive! You could update a "last_seen" timestamp here
                         }
                     });
             }, stream_);
             */
            
            boost::beast::websocket::stream_base::timeout opt;
            opt.handshake_timeout = std::chrono::seconds(30);   // Time to finish handshake
            opt.idle_timeout = std::chrono::minutes(5);         // Close if no data for 5 mins
            opt.keep_alive_pings = true;                        // Send pings automatically
            opt.suggested(boost::beast::role_type::server);
            
            // Apply the options to the stream using your visit pattern
            std::visit([&opt, &req, router, this](auto& ws)
            {
                // Set maximum read message
                ws.read_message_max(16 * 1024 * 1024);
                
                //Set timeout options
                ws.set_option(opt);
                
                // Set a decorator to change the server name
                ws.set_option(boost::beast::websocket::stream_base::decorator([](boost::beast::websocket::response_type& res)
                 {
                     res.set(boost::beast::http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " CppGate Server");
                 }));
                
                // Accept the websocket handshake
                ws.async_accept(req, boost::beast::bind_front_handler(&WebSocketSession::on_accept, shared_from_this(), router));
            }, stream_);
            
            /*
            for (auto& h : req.base()) {
                std::cout << "    " << h.name_string() << "=" << h.value() << "\n";
            }
            std::cout << req.target() << std::endl;
            */
            
            //target_ = std::string(req["Origin"]) + std::string(req.target());
        }

        void on_accept(std::shared_ptr<gtvr::router::Router> router, boost::beast::error_code ec)
        {
            if (ec)
            {
                slog_error("{}", ec.message());
                return;
            }

            //OnOpen event
            router->route(shared_from_this());
            
            do_read();
        }

        void do_read()
        {
            // Read a message into our buffer
            std::visit([this](auto& ws)
            {
                ws.async_read(buffer_, boost::beast::bind_front_handler(&WebSocketSession::on_read, shared_from_this()));
            }, stream_);
        }

        void on_read(boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);

            // This indicates that the websocket_session was closed
            if (ec == boost::beast::websocket::error::closed)
            {
                OnClose();
                return;
            }

            if (ec)
            {
                OnError();
                slog::error("read: {0}.", ec.to_string());
                return;
            }
            
            std::visit([this](auto& ws)
            {
                OnMessage(buffer_.data().data(), buffer_.size(), ws.got_text());
            }, stream_);

            // Clear the buffer
            buffer_.consume(buffer_.size());

            do_read();
        }

        // Returns `true` if the caller may initiate a new read
        bool do_write()
        {
            bool const was_full = write_queue_.size() == queue_limit;

            if (!write_queue_.empty())
            {
                auto& data = write_queue_.front();

                std::visit([this, data](auto& ws) {
                    ws.text(data.is_text);
                    ws.async_write(boost::beast::net::buffer(*data.data), boost::beast::bind_front_handler(&WebSocketSession::on_write, shared_from_this()));
                }, stream_);
            }

            return was_full;
        }

        void on_write(boost::beast::error_code ec, std::size_t bytes_transferred)
        {
            boost::ignore_unused(bytes_transferred);

            if (ec)
            {
                slog_error("{}", ec.message());
                return;
            }

            write_queue_.pop_front();
            
            // Inform the queue that a write completed
            if (do_write())
            {
                //
            }
        }

        void on_close(boost::beast::error_code ec)
        {
            if (ec)
            {
                // This is common if the TCP connection dropped
                // before the WebSocket close handshake finished.
                slog_error("{}", ec.message());
                return;
            }


            // At this point, the WebSocket handshake is complete.
            // The TCP connection will be closed automatically by Beast
            // or you can close the lowest layer manually to be certain.
            //std::cout << "Session closed cleanly." << std::endl;

            // IMPORTANT: Once this function ends, if no other async
            // operations are pending, the shared_ptr 'self' inside
            // the handler dies, and the session object is deleted.
        }
    
        void fail_ex(boost::beast::error_code ec, char const* what)
        {
            // Filter out errors that aren't actually "failures"
            // operation_aborted happens when we close the socket manually
            if (ec == boost::beast::net::error::operation_aborted || ec == boost::beast::websocket::error::closed) {
                return;
            }

            // Log the error
            //std::cerr << "CppGate Error [" << what << "]: " << ec.message() << std::endl;

            // The "Kill Switch": Slam the TCP socket shut
            // This ensures the OS releases the file descriptor immediately.
            std::visit([](auto& ws) {
                boost::beast::error_code ec_ignore;
                
                // We use the 'socket' layer directly to shut it down
                boost::beast::get_lowest_layer(ws).socket().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec_ignore);
                boost::beast::get_lowest_layer(ws).close();
            }, stream_);
        }
    
        inline boost::beast::net::any_io_executor get_strand()
        {
            boost::beast::net::any_io_executor strand;
            
            std::visit([&strand](auto& ws) {
                strand = ws.get_executor();
            }, stream_);
            
            return strand;
        }
    
    public:
    
        WebSocketSession(boost::beast::tcp_stream stream) requires (with_plain): stream_(boost::beast::websocket::stream<boost::beast::tcp_stream>(std::move(stream)))
        {            
        }

        WebSocketSession(boost::beast::ssl_stream<boost::beast::tcp_stream> stream) requires (with_ssl): stream_(boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>(std::move(stream)))
        {            
        }

        ~WebSocketSession()
        {
        }

        // Start the asynchronous operation 
        template<class Body, class Allocator>
        void run(boost::beast::http::request<Body, boost::beast::http::basic_fields<Allocator>> request, std::shared_ptr<gtvr::router::Router> router)
        {
            // Accept the WebSocket upgrade request
            do_accept(std::move(request), router);
        }

        inline boost::beast::http::request<boost::beast::http::string_body>& getRequest()
        {
            return request_;
        }

        virtual void Close()
        {
            boost::beast::net::post(get_strand(), [this, self = shared_from_this()]() {
                std::visit([this](auto& ws) {
                    if (!ws.is_open())
                        return;
                    
                    boost::beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(60));
                    
                    ws.async_close(boost::beast::websocket::close_code::normal, boost::beast::bind_front_handler(&WebSocketSession::on_close, shared_from_this()));
                }, stream_);
            });
        }

    
        void send_internal(std::shared_ptr<const std::string> msg, bool is_text)
        {
            // We must move to the strand to protect the queue
            boost::beast::net::post(get_strand(), [this, self = shared_from_this(), msg = std::move(msg), is_text]() mutable {
                
                // Check for "Slow Consumer" backpressure
                if (write_queue_.size() > queue_limit) {
                    // Optional: Close connection or drop message
                    return;
                }

                // Add to queue
                write_queue_.emplace_back(std::move(msg), is_text);

                // Start the write loop if idle
                if (write_queue_.size() == 1) {
                    do_write();
                }
            });
        }
    
        virtual void SendText(std::shared_ptr<const std::string> data)
        {
            send_internal(data, true);
        }
    
        virtual void SendText(std::string data)
        {
            send_internal(std::make_shared<std::string>(data), true);
        }

        virtual void SendBinary(std::vector<uint8_t> data)
        {
            send_internal(std::make_shared<std::string>(data.begin(), data.end()), false);
        }
};

}//namespace gtvr

