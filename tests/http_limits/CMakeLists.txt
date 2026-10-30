cmake_minimum_required(VERSION 3.20)

# set the project name
project(test_http_limits)

# add the executable
add_executable(test_http_limits test_http_limits.cpp)

if(MSVC)
    target_compile_definitions(test_http_limits PRIVATE -D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    target_compile_options(test_http_limits PRIVATE /bigobj)
endif()

target_link_libraries(test_http_limits PRIVATE cppgate)
target_link_libraries(test_http_limits PRIVATE Catch2::Catch2WithMain)

# cppgate links these privately, but its public headers include them
target_link_libraries(test_http_limits PRIVATE Boost::headers Boost::url)
target_link_libraries(test_http_limits PRIVATE openssl::openssl)
target_link_libraries(test_http_limits PRIVATE re2::re2)
target_link_libraries(test_http_limits PRIVATE spdlog::spdlog_header_only)

catch_discover_tests(test_http_limits)
