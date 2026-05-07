cmake_minimum_required(VERSION 3.16)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
project(crypto_hft VERSION 1.0.0 LANGUAGES CXX)

# ═══════════════════════════════════════════════════════════════════════════
# C++ Standard
# ═══════════════════════════════════════════════════════════════════════════
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ═══════════════════════════════════════════════════════════════════════════
# Dependencies
# ═══════════════════════════════════════════════════════════════════════════

# nlohmann_json (for JSON parsing)
find_package(nlohmann_json 3.9 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found, fetching from GitHub...")
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.2
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# Threading
find_package(Threads REQUIRED)

option(BUILD_PLATFORM "Build the live trading executable" ON)
option(BUILD_TESTS "Build test executables" OFF)

if(BUILD_PLATFORM OR BUILD_TESTS)
    # Boost (Beast is header-only; prefer BoostConfig.cmake targets)
    if(POLICY CMP0167)
        cmake_policy(SET CMP0167 OLD)
    endif()
    find_package(Boost 1.70 REQUIRED)
    message(STATUS "Boost version: ${Boost_VERSION}")

    # OpenSSL (required for SSL/TLS)
    find_package(OpenSSL REQUIRED)
    message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Storage Library
# ═══════════════════════════════════════════════════════════════════════════
add_library(hft_storage
    src/storage/storage_types.cpp
    src/storage/in_memory_storage.cpp
    src/storage/jsonl_async_storage.cpp
)

target_include_directories(hft_storage
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hft_storage
    PUBLIC
        nlohmann_json::nlohmann_json
        Threads::Threads
)

if(BUILD_PLATFORM OR BUILD_TESTS)
    add_library(hft_exchange
        src/exchange/market_data_client.cpp
    )

    target_include_directories(hft_exchange
        PUBLIC
            ${CMAKE_SOURCE_DIR}/include
    )

    target_link_libraries(hft_exchange
        PUBLIC
            Boost::headers
            OpenSSL::SSL
            OpenSSL::Crypto
            nlohmann_json::nlohmann_json
            Threads::Threads
    )
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Main Executable
# ═══════════════════════════════════════════════════════════════════════════
if(BUILD_PLATFORM)
    add_executable(hft_platform
        src/main.cpp
    )

    target_include_directories(hft_platform
        PRIVATE
            ${CMAKE_SOURCE_DIR}/include
    )

    # Compiler flags (optimized for HFT)
    target_compile_options(hft_platform PRIVATE
        $<$<CONFIG:Release>:-O3 -march=native -mtune=native>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -march=native>
        $<$<CONFIG:Debug>:-O0 -g -fsanitize=address,undefined>
    )

    target_compile_definitions(hft_platform PRIVATE
        $<$<CONFIG:Release>:NDEBUG>
        $<$<CONFIG:RelWithDebInfo>:NDEBUG>
    )

    # If you enable sanitizers, you also need the linker flags:
    target_link_options(hft_platform PRIVATE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )

    target_link_libraries(hft_platform
        PRIVATE
            hft_storage
            hft_exchange
            nlohmann_json::nlohmann_json
            Threads::Threads
    )
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Enable LTO for Release builds
# ═══════════════════════════════════════════════════════════════════════════
if(BUILD_PLATFORM AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
    if(lto_supported)
        set_property(TARGET hft_platform PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "LTO enabled")
    else()
        message(STATUS "LTO not supported: ${lto_error}")
    endif()
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Tests (optional)
# ═══════════════════════════════════════════════════════════════════════════
if(BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(test_storage tests/test_storage.cpp)
    target_link_libraries(test_storage
        PRIVATE
            hft_storage
            GTest::gtest_main
    )

    add_executable(test_strategy_runtime tests/test_strategy_runtime.cpp)
    target_link_libraries(test_strategy_runtime
        PRIVATE
            hft_storage
            GTest::gtest_main
    )

    add_executable(test_exchange_api tests/test_exchange_api.cpp)
    target_link_libraries(test_exchange_api
        PRIVATE
            hft_exchange
            GTest::gtest_main
    )

    add_executable(test_backtest tests/test_backtest.cpp)
    target_link_libraries(test_backtest
        PRIVATE
            hft_storage
            GTest::gtest_main
    )

    add_executable(test_orderbook_risk tests/test_orderbook_risk.cpp)
    target_link_libraries(test_orderbook_risk
        PRIVATE
            hft_storage
            GTest::gtest_main
    )

    add_executable(test_ipc tests/test_ipc.cpp)
    target_include_directories(test_ipc
        PRIVATE
            ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(test_ipc
        PRIVATE
            nlohmann_json::nlohmann_json
            Threads::Threads
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(test_storage)
    gtest_discover_tests(test_strategy_runtime)
    gtest_discover_tests(test_exchange_api)
    gtest_discover_tests(test_backtest)
    gtest_discover_tests(test_orderbook_risk)
    gtest_discover_tests(test_ipc)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Installation
# ═══════════════════════════════════════════════════════════════════════════
if(BUILD_PLATFORM)
    install(TARGETS hft_platform RUNTIME DESTINATION bin)
endif()

install(DIRECTORY include/
    DESTINATION include/crypto_hft
    FILES_MATCHING PATTERN "*.hpp"
)

# ═══════════════════════════════════════════════════════════════════════════
# Summary
# ═══════════════════════════════════════════════════════════════════════════
message(STATUS "")
message(STATUS "════════════════════════════════════════════════════════")
message(STATUS "  Crypto HFT Platform Configuration")
message(STATUS "════════════════════════════════════════════════════════")
message(STATUS "  CMake version:    ${CMAKE_VERSION}")
message(STATUS "  C++ compiler:     ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install prefix:   ${CMAKE_INSTALL_PREFIX}")
message(STATUS "════════════════════════════════════════════════════════")
message(STATUS "")
