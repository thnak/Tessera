# SPDX-License-Identifier: MIT
#
# Service-resolve microbenchmark (ADR-050 section 3). Off by default and never registered with
# ctest. Build it from the repo with -DTESSERA_BUILD_SERVICE_RESOLVE_BENCH=ON, or standalone:
#   cmake -S tools/poc/service_resolve -B <dir> -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake_minimum_required(VERSION 3.25)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(tessera_service_resolve_bench LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    if (MSVC)
        add_compile_options(/W4 /WX /permissive- /utf-8)
    else()
        add_compile_options(-Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

add_executable(service_resolve_bench
    main.cpp)
