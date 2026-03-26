#pragma once

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#define FMT_HEADER_ONLY
#include <memory>
#include <spdlog/spdlog.h>

#ifdef __ANDROID__
    #include "spdlog/sinks/android_sink.h"
#else
    #include <spdlog/sinks/stdout_color_sinks.h>
#endif

namespace gtvr {

    class Logger
    {
        private:
            static std::shared_ptr<spdlog::logger> logger;

        public:
            inline static std::shared_ptr<spdlog::logger> Get()
            {
                return logger;
            }

            static void Init()
            {
                #ifdef __ANDROID__
                    logger = spdlog::android_logger_mt("gotoviar", "gtvrmodule");
                #else
                    logger = spdlog::stdout_color_mt("gtvr");
                #endif

                spdlog::set_default_logger(logger);
                spdlog::set_level(spdlog::level::trace);
                //spdlog::set_pattern("[%T.%e][%=8l][%n][%P][%t][%s][%#][%!] %v");
                //spdlog::set_pattern("[%T.%e][%=8l][%n][%P][%t] %s %v");

                #ifdef __EMSCRIPTEN__
                    spdlog::set_pattern("[%L] %s %v");
                #endif

                #ifdef __ANDROID__
                    spdlog::set_pattern("%v");
                #endif
            }
    };

}//namespace gtvr

namespace slog = spdlog;

#define slog_trace(...) SPDLOG_LOGGER_TRACE(gtvr::Logger::Get(), __VA_ARGS__)
#define slog_debug(...) SPDLOG_LOGGER_DEBUG(gtvr::Logger::Get(), __VA_ARGS__)
#define slog_info(...) SPDLOG_LOGGER_INFO(gtvr::Logger::Get(), __VA_ARGS__)
#define slog_warn(...) SPDLOG_LOGGER_WARN(gtvr::Logger::Get(), __VA_ARGS__)
#define slog_error(...) SPDLOG_LOGGER_ERROR(gtvr::Logger::Get(), __VA_ARGS__)
#define slog_critical(...) SPDLOG_LOGGER_CRITICAL(gtvr::Logger::Get(), __VA_ARGS__)


#ifdef __EMSCRIPTEN__
    #if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE
        #define slog_em_trace(js_obj, ...) slog_trace(__VA_ARGS__);emscripten::val::global("console").call<void>("log", js_obj)
    #else
        #define slog_em_trace(logger, ...) (void)0
    #endif

    #if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_DEBUG
        #define slog_em_debug(js_obj, ...) slog_debug(__VA_ARGS__);emscripten::val::global("console").call<void>("log", js_obj)
    #else
        #define slog_em_debug(logger, ...) (void)0
    #endif

    #if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_INFO
        #define slog_em_info(js_obj, ...) slog_info(__VA_ARGS__);emscripten::val::global("console").call<void>("log", js_obj)
    #else
        #define slog_em_info(logger, ...) (void)0
    #endif

    #if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_WARN
        #define slog_em_warn(js_obj, ...) slog_warn(__VA_ARGS__);emscripten::val::global("console").call<void>("log", js_obj)
    #else
        #define slog_em_warn(logger, ...) (void)0
    #endif

    #if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_ERROR
        #define slog_em_error(js_obj, ...) slog_error(__VA_ARGS__);emscripten::val::global("console").call<void>("log", js_obj)
    #else
        #define slog_em_error(logger, ...) (void)0
    #endif

    #if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_CRITICAL
        #define slog_em_critical(js_obj, ...) slog_critical(__VA_ARGS__);emscripten::val::global("console").call<void>("log", js_obj)
    #else
        #define slog_em_critical(logger, ...) (void)0
    #endif
#endif
