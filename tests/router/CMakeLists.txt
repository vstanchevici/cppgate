cmake_minimum_required(VERSION 3.20)

# set the project name
project(test_router)

# add the executable
add_executable(test_router test_router.cpp)

target_sources(test_router PRIVATE ${FilesNetwork})
target_include_directories(test_router PRIVATE ${SOURCE_ROOT_PATH})
target_include_directories(test_router PRIVATE ${GENERATED_INCLUDE_PATH})

if(MSVC)
    target_compile_definitions(test_router PRIVATE -D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    target_compile_options(test_router PRIVATE /bigobj)
    catch_discover_tests(test_router)
endif()

target_link_libraries(test_router PRIVATE Catch2::Catch2WithMain)

target_link_libraries(test_router PRIVATE Boost::headers Boost::url)
target_link_libraries(test_router PRIVATE openssl::openssl)
target_link_libraries(test_router PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(test_router PRIVATE argparse::argparse)
target_link_libraries(test_router PRIVATE yaml-cpp)
target_link_libraries(test_router PRIVATE spdlog::spdlog_header_only)
target_link_libraries(test_router PRIVATE re2::re2)
target_link_libraries(test_router PRIVATE jwt-cpp::jwt-cpp)


