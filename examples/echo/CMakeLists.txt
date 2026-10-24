cmake_minimum_required(VERSION 3.20)

# set the project name
project(echo_server)

# add the executable
add_executable(echo_server main.cpp)

if(MSVC)
    target_compile_definitions(echo_server PRIVATE -D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    target_compile_options(echo_server PRIVATE /bigobj)
endif()

target_link_libraries(echo_server PRIVATE cppgate)

# cppgate links these privately, but its public headers include them
target_link_libraries(echo_server PRIVATE Boost::headers Boost::url)
target_link_libraries(echo_server PRIVATE openssl::openssl)
target_link_libraries(echo_server PRIVATE re2::re2)
target_link_libraries(echo_server PRIVATE spdlog::spdlog_header_only)
