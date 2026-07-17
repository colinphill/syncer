cmake_minimum_required(VERSION 3.22)

project(syncer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SYNCER_BUILD_TESTS "Build host-side unit tests" ON)

include(FetchContent)
set(_syncer_boost_components headers asio uuid)
set(BOOST_INCLUDE_LIBRARIES asio uuid)
if(WIN32 AND NOT ANDROID)
    list(APPEND _syncer_boost_components program_options json)
    list(APPEND BOOST_INCLUDE_LIBRARIES program_options json)
endif()
find_package(Boost 1.81 CONFIG QUIET COMPONENTS ${_syncer_boost_components})
if(NOT Boost_FOUND)
    set(BOOST_ENABLE_CMAKE ON)
    FetchContent_Declare(Boost
        URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
        URL_HASH SHA256=2c5ec5edcdff47ff55e27ed9560b0a0b94b07bd07ed9928b476150e16b0efc57
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(Boost)
endif()

add_library(syncer_core STATIC
    src/common/blake3.cpp
    src/common/glob.cpp
    src/common/manifest.cpp
    src/common/protocol.cpp)
target_include_directories(syncer_core PUBLIC include)
target_link_libraries(syncer_core PUBLIC Boost::headers Boost::asio Boost::uuid)
target_compile_definitions(syncer_core PUBLIC
    SYNCER_VERSION="${PROJECT_VERSION}"
    SYNCER_PROTOCOL_VERSION=2)

if(MSVC)
    target_compile_options(syncer_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(syncer_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32 AND NOT ANDROID)
    target_link_libraries(syncer_core PUBLIC ws2_32)
    add_executable(syncer
        src/client/adb.cpp
        src/client/client.cpp
        src/client/main.cpp
        src/client/process_windows.cpp
        src/client/scanner.cpp)
    target_link_libraries(syncer PRIVATE syncer_core Boost::program_options Boost::json)
    target_compile_definitions(syncer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(syncer PRIVATE /W4 /permissive- /EHsc)
    endif()
endif()

if(ANDROID)
    add_executable(syncerd src/server/main.cpp src/server/server.cpp)
    target_link_libraries(syncerd PRIVATE syncer_core log)
    target_compile_options(syncerd PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SYNCER_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(syncer_tests tests/tests.cpp)
    target_link_libraries(syncer_tests PRIVATE syncer_core)
    if(WIN32)
        target_sources(syncer_tests PRIVATE src/client/scanner.cpp)
        target_include_directories(syncer_tests PRIVATE src/client)
        target_compile_definitions(syncer_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    add_test(NAME syncer_tests COMMAND syncer_tests)
endif()
