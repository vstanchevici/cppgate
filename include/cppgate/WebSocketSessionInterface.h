#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace gtvr {

    struct WebSocketSessionInterface
    {
        private:
            //std::function_ref<void(void* buffer, size_t size, bool is_text)> onClose;
            //std::function_ref<void()> onClose;
            //std::function_ref<void()> onError;

        public:
            virtual void OnMessage(void* buffer, size_t size, bool is_text) {};

            virtual void OnClose() {};

            virtual void OnError() {};

            virtual boost::beast::http::request<boost::beast::http::string_body>& getRequest() = 0;

            virtual void Close() = 0;

            virtual void SendText(std::string data) = 0;
        
            virtual void SendText(std::shared_ptr<const std::string> data) = 0;
        
            virtual void SendBinary(std::vector<uint8_t> data) = 0;
    };

}//namespace gtvr
