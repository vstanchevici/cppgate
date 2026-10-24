cmake_minimum_required(VERSION 3.20)

# set the project name
project(test_trusted_proxies)

# add the executable
add_executable(test_trusted_proxies test_trusted_proxies.cpp)

if(MSVC)
    target_compile_definitions(test_trusted_proxies PRIVATE -D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    target_compile_options(test_trusted_proxies PRIVATE /bigobj)
endif()

target_link_libraries(test_trusted_proxies PRIVATE cppgate)
target_link_libraries(test_trusted_proxies PRIVATE Catch2::Catch2WithMain)

# cppgate links these privately, but its public headers include them
target_link_libraries(test_trusted_proxies PRIVATE Boost::headers Boost::url)
target_link_libraries(test_trusted_proxies PRIVATE re2::re2)
target_link_libraries(test_trusted_proxies PRIVATE spdlog::spdlog_header_only)

catch_discover_tests(test_trusted_proxies)
