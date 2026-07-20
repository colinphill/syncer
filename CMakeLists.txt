cmake_minimum_required(VERSION 3.22)

project(syncer VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SYNCER_BUILD_TESTS "Build host-side unit tests" ON)

include(FetchContent)
set(_syncer_boost_components headers asio uuid)
set(BOOST_INCLUDE_LIBRARIES asio uuid)
if(NOT ANDROID)
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
    SYNCER_PROTOCOL_VERSION=6)

if(MSVC)
    target_compile_options(syncer_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(syncer_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NOT ANDROID)
    find_package(Threads REQUIRED)
    if(WIN32)
        target_link_libraries(syncer_core PUBLIC ws2_32)
        set(_syncer_process_source src/client/process_windows.cpp)
        set(_syncer_scanner_source src/client/scanner.cpp)
    elseif(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_syncer_process_source src/client/process_posix.cpp)
        set(_syncer_scanner_source src/client/scanner_posix.cpp)
    else()
        message(FATAL_ERROR "The syncer client supports Windows, macOS, and Linux")
    endif()
    add_executable(syncer
        src/client/adb.cpp
        src/client/client.cpp
        src/client/main.cpp
        ${_syncer_process_source}
        ${_syncer_scanner_source})
    target_link_libraries(syncer PRIVATE syncer_core Boost::program_options Boost::json Threads::Threads)
    if(WIN32)
        target_compile_definitions(syncer PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
            _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    endif()
    if(MSVC)
        target_compile_options(syncer PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(syncer PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(ANDROID)
    add_executable(syncerd src/server/main.cpp src/server/server.cpp)
    target_link_libraries(syncerd PRIVATE syncer_core log)
    target_compile_options(syncerd PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SYNCER_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(syncer_tests tests/tests.cpp ${_syncer_process_source} ${_syncer_scanner_source})
    target_include_directories(syncer_tests PRIVATE src/client)
    target_compile_definitions(syncer_tests PRIVATE SYNCER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(syncer_tests PRIVATE syncer_core Threads::Threads)
    if(WIN32)
        target_compile_definitions(syncer_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(syncer_tests PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(syncer_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME syncer_tests COMMAND syncer_tests)
endif()
