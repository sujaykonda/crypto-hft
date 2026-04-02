cmake_minimum_required(VERSION 3.16)
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

# Boost (Beast is header-only; prefer BoostConfig.cmake targets)
# Homebrew Boost 1.90 works best this way.
find_package(Boost 1.70 REQUIRED)  # raise/lower if you want; 1.70+ has good targets

message(STATUS "Boost version: ${Boost_VERSION}")

# OpenSSL (required for SSL/TLS)
find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")

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

# ═══════════════════════════════════════════════════════════════════════════
# Main Executable
# ═══════════════════════════════════════════════════════════════════════════
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
        Boost::headers          # Beast/Asio headers
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        Threads::Threads
)

# ═══════════════════════════════════════════════════════════════════════════
# Enable LTO for Release builds
# ═══════════════════════════════════════════════════════════════════════════
if(CMAKE_BUILD_TYPE STREQUAL "Release")
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
option(BUILD_TESTS "Build test executables" OFF)
if(BUILD_TESTS)
    enable_testing()

    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "GTest not found, fetching from GitHub...")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        FetchContent_MakeAvailable(googletest)
    endif()

    add_executable(test_storage tests/test_storage.cpp)
    target_link_libraries(test_storage
        PRIVATE
            hft_storage
            GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(test_storage)
endif()

# ═══════════════════════════════════════════════════════════════════════════
# Installation
# ═══════════════════════════════════════════════════════════════════════════
install(TARGETS hft_platform RUNTIME DESTINATION bin)

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
