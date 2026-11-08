# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.24)
project(Tessera VERSION 0.0.1 LANGUAGES C CXX)

# Static linking is a hard requirement (ADR-012).
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(TESSERA_STATIC_RUNTIME
    "Link Tessera and the C/C++ runtime statically where supported" ON)

if (MSVC AND TESSERA_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if (TESSERA_STATIC_RUNTIME AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(cmake/CompilerCache.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TESSERA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if (MSVC)
    add_compile_options(/W4 /permissive-)
    if (TESSERA_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if (TESSERA_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

option(TESSERA_BUILD_POC "Build Phase 0 proof-of-concept tools" ON)
option(TESSERA_BUILD_TESTS "Build unit tests" ON)
option(TESSERA_BUILD_VULKAN "Build the Vulkan backend and PoC" ON)

add_subdirectory(third_party)
add_subdirectory(src/rendering/rhi)
add_subdirectory(src/rendering/gpu)
add_subdirectory(src/rendering/damage)
add_subdirectory(src/rendering/null)
add_subdirectory(src/runtime)
add_subdirectory(src/ui/input)
add_subdirectory(src/ui/scene)
add_subdirectory(src/ui/style)
add_subdirectory(src/ui/text)
add_subdirectory(src/ui/animation)
add_subdirectory(src/ui/layout)
add_subdirectory(src/compiler/merger)

if (TESSERA_BUILD_POC)
    add_subdirectory(tools/poc/merger)
endif()

if (TESSERA_BUILD_VULKAN)
    add_subdirectory(src/rendering/backend/vulkan)
    if (TESSERA_BUILD_POC)
        add_subdirectory(tools/poc/vulkan_offscreen)
        add_subdirectory(tools/poc/batched2d)
    endif()
endif()

if (TESSERA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    add_subdirectory(tests/gpu)
endif()
