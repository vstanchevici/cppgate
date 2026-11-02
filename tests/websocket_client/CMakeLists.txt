cmake_minimum_required(VERSION 3.20)

# set the project name
project(test_websocket_client)

# add the executable
add_executable(test_websocket_client test_websocket_client.cpp)

if(MSVC)
    target_compile_definitions(test_websocket_client PRIVATE -D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    target_compile_options(test_websocket_client PRIVATE /bigobj)
endif()

target_link_libraries(test_websocket_client PRIVATE cppgate)
target_link_libraries(test_websocket_client PRIVATE Catch2::Catch2WithMain)

# cppgate links these privately, but its public headers include them
target_link_libraries(test_websocket_client PRIVATE Boost::headers Boost::url)
target_link_libraries(test_websocket_client PRIVATE openssl::openssl)
target_link_libraries(test_websocket_client PRIVATE re2::re2)
target_link_libraries(test_websocket_client PRIVATE spdlog::spdlog_header_only)

catch_discover_tests(test_websocket_client)
