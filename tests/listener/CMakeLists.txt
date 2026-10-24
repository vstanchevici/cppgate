cmake_minimum_required(VERSION 3.20)

# set the project name
project(test_listener)

# add the executable
add_executable(test_listener test_listener.cpp)

if(MSVC)
    target_compile_definitions(test_listener PRIVATE -D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    target_compile_options(test_listener PRIVATE /bigobj)
endif()

target_link_libraries(test_listener PRIVATE cppgate)
target_link_libraries(test_listener PRIVATE Catch2::Catch2WithMain)

# cppgate links these privately, but its public headers include them
target_link_libraries(test_listener PRIVATE Boost::headers Boost::url)
target_link_libraries(test_listener PRIVATE openssl::openssl)
target_link_libraries(test_listener PRIVATE re2::re2)
target_link_libraries(test_listener PRIVATE spdlog::spdlog_header_only)

catch_discover_tests(test_listener)
