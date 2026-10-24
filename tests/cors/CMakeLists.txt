cmake_minimum_required(VERSION 3.20)

# set the project name
project(test_cors)

# add the executable
add_executable(test_cors test_cors.cpp)

if(MSVC)
    target_compile_definitions(test_cors PRIVATE -D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    target_compile_options(test_cors PRIVATE /bigobj)
endif()

target_link_libraries(test_cors PRIVATE cppgate)
target_link_libraries(test_cors PRIVATE Catch2::Catch2WithMain)

# cppgate links these privately, but its public headers include them
target_link_libraries(test_cors PRIVATE Boost::headers Boost::url)
target_link_libraries(test_cors PRIVATE re2::re2)
target_link_libraries(test_cors PRIVATE spdlog::spdlog_header_only)

catch_discover_tests(test_cors)
