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

# /Z7 embeds debug info in the object file, which compiler caches (sccache/ccache) can cache;
# /Zi writes a shared PDB and defeats them. Needs CMake 3.25+ (CMP0141); older CMake ignores it.
if (MSVC AND NOT CMAKE_VERSION VERSION_LESS 3.25)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
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
    # /utf-8: source files are UTF-8 without a BOM (test fixtures carry non-ASCII literals like
    # U'א'); without it MSVC reads them as the ANSI code page and rejects multi-byte constants.
    add_compile_options(/W4 /permissive- /utf-8)
    # The Visual Studio generator compiles the files of one project serially unless /MP is set.
    # Ninja already parallelizes per file, so /MP there would only oversubscribe the CPU.
    if (CMAKE_GENERATOR MATCHES "Visual Studio")
        add_compile_options(/MP)
    endif()
    if (TESSERA_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if (TESSERA_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# Emscripten disables exception catching by default (a throw aborts). Tessera throws at boundaries
# and catches there, so every translation unit and the link use native Wasm exceptions.
if (EMSCRIPTEN)
    add_compile_options(-fwasm-exceptions)
    add_link_options(-fwasm-exceptions)
endif()

option(TESSERA_BUILD_POC "Build Phase 0 proof-of-concept tools" ON)
option(TESSERA_BUILD_TESTS "Build unit tests" ON)
option(TESSERA_BUILD_VULKAN "Build the Vulkan backend and PoC" ON)
option(TESSERA_BUILD_WEBGPU
    "Build the WebGPU device backend against a native webgpu.h (always built under Emscripten)" OFF)
option(TESSERA_SHADER_CROSS
    "Cross-compile shaders to MSL/HLSL/WGSL at build time and validate them (ADR-049; needs tools)"
    OFF)
option(TESSERA_ENABLE_THORVG
    "Build the ThorVG-backed vector path rasterizer (D1, ADR-004)" OFF)
# Span tracing is a development and automation feature, never a production one (ADR-022 decision
# 11, docs/specs/diagnostics-config-v1.md section 5). OFF compiles no recorder at all: the
# TraceRecorder in the header becomes an empty no-op and collect_trace() still answers with the
# documented empty Chrome document, so no tracing bytecode or JSON text reaches a shipped binary.
# Dev, CI and the automation lanes turn it ON; release/packaging leaves it OFF.
option(TESSERA_ENABLE_TRACING
    "Compile the trace recorder and Chrome trace exporter (dev, CI and automation builds)" OFF)

# X11 backs the Linux window and the Vulkan Xlib present surface. Without it (and without
# Win32) the HAL keeps the headless stub and the Vulkan device reports NotSupported for
# presentation, so headless Linux builds still work.
set(TESSERA_X11_FOUND OFF)
if (UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)
    find_package(X11 QUIET)
    if (X11_FOUND)
        set(TESSERA_X11_FOUND ON)
        message(STATUS "X11 found: Linux window and Vulkan Xlib surface enabled")
    else()
        message(STATUS "X11 not found: Linux window/presentation disabled (stub)")
    endif()
endif()

add_subdirectory(third_party)
add_subdirectory(src/rendering/rhi)
add_subdirectory(src/rendering/gpu)
add_subdirectory(src/rendering/damage)
add_subdirectory(src/rendering/golden)
add_subdirectory(src/rendering/null)
add_subdirectory(src/rendering/path)
add_subdirectory(src/rendering/image)
add_subdirectory(src/rendering/software)
add_subdirectory(src/diagnostics)
add_subdirectory(src/hal/timer)
add_subdirectory(src/hal/window)
add_subdirectory(src/runtime)
add_subdirectory(src/ui/input)
add_subdirectory(src/ui/scene)
add_subdirectory(src/ui/style)
add_subdirectory(src/ui/scale)
add_subdirectory(src/ui/text)
add_subdirectory(src/ui/animation)
add_subdirectory(src/ui/layout)
add_subdirectory(src/ui/frame)
add_subdirectory(src/ui/paint)
add_subdirectory(src/ui/assets)
add_subdirectory(src/ui/accessibility)
add_subdirectory(src/ui/automation)
add_subdirectory(src/ui/virtual)
add_subdirectory(src/ui/boundary)
add_subdirectory(src/app)
add_subdirectory(src/harness)
add_subdirectory(src/schema)
add_subdirectory(src/compiler/merger)
add_subdirectory(src/compiler/analyzer)
add_subdirectory(src/compiler/ir)
add_subdirectory(src/compiler/eval)
add_subdirectory(src/compiler/ids)
add_subdirectory(src/compiler/rules)
add_subdirectory(src/compiler/lowering)
add_subdirectory(src/compiler/codegen)
add_subdirectory(tools/schema_dump)
# `tessera icons add` (ADR-047 item 8): build-time Iconify import with the license gate.
add_subdirectory(tools/icons)

# docs/specs/shader-manifest-v1.md sections 1 and 5: the manifest is a repository build input and
# `tessera_schema` validates it before glslc. The checker target must exist before the Vulkan
# shader rules reference it, so it is defined here; the cross-compile rules (and their ctest
# checks) are added after enable_testing() below.
set(TESSERA_SHADER_MANIFEST "${PROJECT_SOURCE_DIR}/tools/shaders/manifest.json")
if (TESSERA_BUILD_VULKAN OR TESSERA_SHADER_CROSS)
    add_subdirectory(tools/shaders)
endif()

if (TESSERA_BUILD_POC)
    add_subdirectory(tools/poc/merger)
endif()

# ADR-050 service-resolve microbenchmark: Release-only measurement tool, never in ctest.
option(TESSERA_BUILD_SERVICE_RESOLVE_BENCH
    "Build the ADR-050 service-resolve microbenchmark (not registered with ctest)" OFF)
if (TESSERA_BUILD_SERVICE_RESOLVE_BENCH)
    add_subdirectory(tools/poc/service_resolve)
endif()

if (TESSERA_BUILD_POC AND TESSERA_ENABLE_THORVG)
    add_subdirectory(tools/poc/thorvg_spike)
endif()

if (TESSERA_BUILD_POC AND TARGET tessera_app_window)
    add_subdirectory(tools/poc/design_mock)
endif()

# WebGPU mapping tables and the WGSL registry are API-free and always built; the device itself is
# gated inside (Emscripten or TESSERA_BUILD_WEBGPU).
add_subdirectory(src/rendering/backend/webgpu)

if (TESSERA_BUILD_VULKAN)
    add_subdirectory(src/rendering/backend/vulkan)
    if (TESSERA_BUILD_POC)
        add_subdirectory(tools/poc/vulkan_offscreen)
        add_subdirectory(tools/poc/batched2d)
        # The generated demo target only exists on host builds, and the sample presents through the
        # Vulkan surface path, so it is gated like the PoCs.
        if (NOT CMAKE_CROSSCOMPILING)
            add_subdirectory(samples/click-demo)
            add_subdirectory(samples/gallery)
        endif()
    endif()
endif()

if (TESSERA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/unit)
    add_subdirectory(tests/gpu)
    # GPU-free benchmark suites (issue #241). Kept in their own subdirectory so the CPU benchmark
    # registers itself without editing the shared unit-test registry.
    add_subdirectory(tests/benchmarks)
    # Integration tests live in their own directory; the parent list stays valid when the folder is
    # empty (a fresh checkout during a lane handoff) so configure never fails on a missing dir.
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/CMakeLists.txt)
        add_subdirectory(tests/integration)
    endif()
    # Preview parity (issue #7): IR evaluator versus generated C++ for a native handler.
    add_subdirectory(tests/parity)
endif()

# ADR-049: translate the portable SPIR-V to MSL/HLSL/WGSL in the build directory and validate it.
# After enable_testing() so its tool checks register with ctest.
if (TESSERA_SHADER_CROSS)
    add_subdirectory(tools/shaders/cross)
endif()

# Ship the project license, third-party notices and the generated license bundle with an install.
# Kept last so the module can see every option and target defined above.
include(cmake/tessera_packaging.cmake)
