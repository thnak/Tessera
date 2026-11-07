cmake_minimum_required(VERSION 3.24)
project(Tessera VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(TESSERA_BUILD_POC "Build Phase 0 proof-of-concept tools" ON)
option(TESSERA_BUILD_TESTS "Build unit tests" ON)
option(TESSERA_BUILD_VULKAN "Build the Vulkan backend and PoC" ON)

add_subdirectory(third_party)
add_subdirectory(src/compiler/merger)

if (TESSERA_BUILD_POC)
    add_subdirectory(tools/poc/merger)
endif()

if (TESSERA_BUILD_VULKAN)
    add_subdirectory(src/rendering/backend/vulkan)
    if (TESSERA_BUILD_POC)
        add_subdirectory(tools/poc/vulkan_offscreen)
    endif()
endif()

if (TESSERA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
endif()
