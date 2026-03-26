 #
# === CMake lists for the backend ===
#


# === CMake options ===

# Set CMake version
cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

# Set policies
cmake_policy(SET CMP0114 NEW)

#
# Configure CMake environment
#

# Register general cmake commands
#include(cmake/Custom.cmake)
include(cmake/Tools.cmake)

# Set policies
#set_policy(CMP0054 NEW) # ENABLE CMP0054: Only interpret if() arguments as variables or keywords when unquoted.
#set_policy(CMP0042 NEW) # ENABLE CMP0042: MACOSX_RPATH is enabled by default.
#set_policy(CMP0063 NEW) # ENABLE CMP0063: Honor visibility properties for all target types.
#set_policy(CMP0077 NEW) # ENABLE CMP0077: option() honors normal variables

# Remove ZERO_CHECK target
#set(CMAKE_SUPPRESS_REGENERATION true)

# Include cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GenerateExportHeader)

# Include custom cmake modules
include(cmake/GenerateTemplateExportHeader.cmake)
include(cmake/GetGitRevisionDescription.cmake)

#Static lib for MSVC
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

#
# === Project description and (meta) information ===
#

# Get git revision
get_git_head_revision(GIT_REFSPEC GIT_SHA1)
string(SUBSTRING "${GIT_SHA1}" 0 12 GIT_REV)
if(NOT GIT_SHA1)
    set(GIT_REV "0")
endif()

# Meta information about the project
set(META_PROJECT_NAME        "cppgate")
set(META_PROJECT_DESCRIPTION "A High-Performance REST & WebSocket Library")
set(META_PROJECT_URL         "https://github.com/vstanchevici/cppgate")
set(META_VERSION_MAJOR       "0")
set(META_VERSION_MINOR       "1")
set(META_VERSION_PATCH       "0")
set(META_VERSION_REVISION    "${GIT_REV}")
set(META_VERSION             "${META_VERSION_MAJOR}.${META_VERSION_MINOR}.${META_VERSION_PATCH}")
set(META_NAME_VERSION        "${META_PROJECT_NAME} v${META_VERSION} git hash: ${GIT_REV}")

string(MAKE_C_IDENTIFIER ${META_PROJECT_NAME} META_PROJECT_ID)
string(TOUPPER ${META_PROJECT_ID} META_PROJECT_ID)


#
# Project configuration options
#

# Project options
#option(OPTION_BUILD_DOCS      "Build documentation."                                   OFF)
#option(OPTION_ENABLE_COVERAGE "Add coverage information."                              OFF)
option(BUILD_TESTING            "Build tests."                                          OFF)
option(BUILD_EXAMPLES           "Build examples."                                       OFF)

# Declare project
#

# Generate folders for IDE targets (e.g., VisualStudio solutions)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(IDE_FOLDER "")

# Declare project
project(${META_PROJECT_NAME} LANGUAGES C CXX)

# Set standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

# Create library
add_library(${PROJECT_NAME})
target_compile_definitions(${PROJECT_NAME} PRIVATE "${PROJECT_NAME}_EXPORTS")

target_compile_definitions(${PROJECT_NAME} PRIVATE "META_PROJECT_NAME=${META_PROJECT_NAME}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "META_PROJECT_DESCRIPTION=${META_PROJECT_DESCRIPTION}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "META_PROJECT_URL=${META_PROJECT_URL}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "META_VERSION_MAJOR=${META_VERSION_MAJOR}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "META_VERSION_MINOR=${META_VERSION_MINOR}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "META_VERSION_PATCH=${META_VERSION_PATCH}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "META_VERSION_REVISION=${META_VERSION_REVISION}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "META_VERSION=${META_VERSION}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "META_NAME_VERSION=${META_NAME_VERSION}")


get_target_property(target_type ${PROJECT_NAME} TYPE)

if (NOT target_type STREQUAL "SHARED_LIBRARY")
    target_compile_definitions(${PROJECT_NAME} PUBLIC CPPGATE_EXPORT_STATIC)
endif()

# Create version file
file(WRITE "${PROJECT_BINARY_DIR}/VERSION" "${META_NAME_VERSION}")


# Set sources root path
set(SOURCE_ROOT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/sources")
set(FilesNetwork)

find_source_files(FilesNetwork                      CXX     "${SOURCE_ROOT_PATH}/Core"
                                                            "${SOURCE_ROOT_PATH}/BoostBeast"
                                                            "${SOURCE_ROOT_PATH}/Authentication"
                                                            "${SOURCE_ROOT_PATH}/Handlers")
                                                            
foreach(_source IN ITEMS ${FilesNetwork})
    get_filename_component(_source_path "${_source}" PATH)
    file(RELATIVE_PATH _source_path_rel "${SOURCE_ROOT_PATH}" "${_source_path}")
    string(REPLACE "/" "\\" _group_path "${_source_path_rel}")
    source_group("sources\\${_group_path}" FILES "${_source}")
endforeach()

target_sources(${PROJECT_NAME} PRIVATE ${FilesNetwork})
target_include_directories(${PROJECT_NAME} PRIVATE ${SOURCE_ROOT_PATH})


# Set includes root path
set(INCLUDE_ROOT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/include/${PROJECT_NAME}")

set(FilesNetwork)
find_source_files(FilesNetwork                      INC     "${INCLUDE_ROOT_PATH}"
                                                            "${INCLUDE_ROOT_PATH}/Core"
                                                            "${INCLUDE_ROOT_PATH}/BoostBeast"
                                                            "${INCLUDE_ROOT_PATH}/Authentication"
                                                            "${INCLUDE_ROOT_PATH}/Handlers")

target_sources(${PROJECT_NAME} PRIVATE ${FilesNetwork})

foreach(_source IN ITEMS ${FilesNetwork})
    get_filename_component(_source_path "${_source}" PATH)
    file(RELATIVE_PATH _source_path_rel "${INCLUDE_ROOT_PATH}" "${_source_path}")
    string(REPLACE "/" "\\" _group_path "${_source_path_rel}")
    message(STATUS ${_group_path})
    source_group("include\\${_group_path}" FILES "${_source}")
endforeach()


target_include_directories(${PROJECT_NAME} PUBLIC 
    # This is for the local build
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    
    # This is for the final CONSUMER (once installed)
    "$<INSTALL_INTERFACE:include>"
)

# Set compiler options
if(MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE -D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS -D_WINSOCK_DEPRECATED_NO_WARNINGS)
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()


# Add libraries
find_package(Boost CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE boost::boost)

# add openssl
set_property(GLOBAL PROPERTY FIND_LIBRARY_USE_LIB64_PATHS TRUE)
find_package(OpenSSL CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE openssl::openssl)

find_package(spdlog CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog_header_only)

find_package(re2 CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE re2::re2)

#find_package(nlohmann_json CONFIG REQUIRED)
#target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)

#find_package(argparse CONFIG REQUIRED)
#target_link_libraries(${PROJECT_NAME} PRIVATE argparse::argparse)

#find_package(yaml-cpp CONFIG REQUIRED)
#target_link_libraries(${PROJECT_NAME} PRIVATE yaml-cpp)

#find_package(jwt-cpp REQUIRED CONFIG)
#target_link_libraries(${PROJECT_NAME} PRIVATE jwt-cpp::jwt-cpp)


# link system libraries
if(UNIX)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${CMAKE_DL_LIBS})
endif()

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 wsock32 crypt32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE -D_WIN32_WINDOWS)
endif()

if (BUILD_EXAMPLES)
    file(GLOB dir_list LIST_DIRECTORIES TRUE "${CMAKE_CURRENT_SOURCE_DIR}/examples/*")
    foreach(dir ${dir_list})
        if(IS_DIRECTORY ${dir} AND NOT ${dir} STREQUAL "Common")
            add_subdirectory(${dir})            
        else()
            continue()
        endif()
    endforeach()
endif()

if (BUILD_TESTING)
    #https://github.com/catchorg/Catch2/blob/devel/docs/cmake-integration.md
    find_package(Catch2 REQUIRED)
    include(CTest)
    include(Catch)

    file(GLOB dir_list LIST_DIRECTORIES TRUE "${CMAKE_CURRENT_SOURCE_DIR}/tests/*")
    foreach(dir ${dir_list})
        if(IS_DIRECTORY ${dir})
            add_subdirectory(${dir})            
        else()
            continue()
        endif()
    endforeach()
endif()

# Tell CMake which files to "Export"
install(TARGETS ${PROJECT_NAME}
    EXPORT cppgateTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

# Create the .cmake files so the Consumer can find it
install(EXPORT cppgateTargets
    FILE cppgateTargets.cmake
    NAMESPACE cppgate::
    DESTINATION lib/cmake/cppgate
)