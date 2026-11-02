cmake_minimum_required(VERSION 3.20)

# set the project name
project(websocket_client_example)

# add the executable
add_executable(websocket_client_example main.cpp)

if(MSVC)
    target_compile_definitions(websocket_client_example PRIVATE -D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    target_compile_options(websocket_client_example PRIVATE /bigobj)
endif()

target_link_libraries(websocket_client_example PRIVATE cppgate)

# cppgate links these privately, but its public headers include them
target_link_libraries(websocket_client_example PRIVATE Boost::headers Boost::url)
target_link_libraries(websocket_client_example PRIVATE openssl::openssl)
target_link_libraries(websocket_client_example PRIVATE re2::re2)
target_link_libraries(websocket_client_example PRIVATE spdlog::spdlog_header_only)
