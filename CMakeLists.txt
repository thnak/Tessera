# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.24)

# /Z7 embeds debug info in the object file, which compiler caches (sccache/ccache) can cache; /Zi
# writes a shared PDB and defeats them. CMP0141 must be set BEFORE project(): that is where CMake
# initializes the per-config debug-format flags, so setting it afterwards leaves the default `/Zi`
# in CMAKE_CXX_FLAGS_DEBUG and the cache stays defeated (every compile then contends on one PDB and
# fails with C1041). Guarded because the policy does not exist before CMake 3.25.
if (POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()

project(Tessera VERSION 0.0.1 LANGUAGES C CXX)

# Static linking is a hard requirement (ADR-012).
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(TESSERA_STATIC_RUNTIME
    "Link Tessera and the C/C++ runtime statically where supported" ON)

if (MSVC AND TESSERA_STATIC_RUNTIME)
    set(_tessera_msvc_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Built under another project (`tessera new`, route B), the parent's own targets are compiled
    # with the parent's runtime and linked against ours. The setting below is directory-scoped, so
    # it cannot reach them: a mismatch is LNK2038 at the very end of a long build. Say so at
    # configure time instead, and name the fix.
    if (NOT PROJECT_IS_TOP_LEVEL AND
        NOT "${CMAKE_MSVC_RUNTIME_LIBRARY}" STREQUAL "${_tessera_msvc_runtime}")
        message(FATAL_ERROR
            "Tessera links the C/C++ runtime statically (ADR-012), so the project that includes it "
            "must too. Before add_subdirectory() or find_package(), set:\n"
            "  set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreaded$<$<CONFIG:Debug>:Debug>\")\n"
            "(`tessera new` projects do this already.) Current value: \"${CMAKE_MSVC_RUNTIME_LIBRARY}\".")
    endif()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "${_tessera_msvc_runtime}")
endif()

# The debug-info format itself, now that the policy above is in force. Embedded (/Z7) is what makes
# an object file self-contained, so a cache hit is a real hit rather than a PDB that must be rebuilt.
if (MSVC)
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
# Internal-padding sweep (issue #731). `-Wpadded` is not part of the default warning set because the
# tree has 236 hits across 105 headers, and turning it on unconditionally would make every lane red.
# It is an opt-in audit instead: configure with -DTESSERA_WARN_PADDING=ON and read the warnings, or
# add -DTESSERA_WARNINGS_AS_ERRORS=OFF to keep them from failing the build. The types that were
# fixed are pinned by `padding_size_tests` and the memory budget, so this flag is for finding the
# next one, not for gating.
option(TESSERA_WARN_PADDING "Warn on internal struct padding (-Wpadded / /w14242)" OFF)

if (MSVC)
    # /utf-8: source files are UTF-8 without a BOM (test fixtures carry non-ASCII literals like
    # U'א'); without it MSVC reads them as the ANSI code page and rejects multi-byte constants.
    add_compile_options(/W4 /permissive- /utf-8)
    # C4062 is "enumerator not handled in switch and there is no default label"; /W4 does not include
    # it, while GCC's and Clang's `-Wswitch` (part of `-Wall`, so on every other lane) do. ADR-066's
    # four steps hit that gap nine times, each cost a full CI round, and each fix was one line
    # (issue #805). `scripts/check_switch_exhaustive.py` catches the same class without a compiler;
    # this flag is the compiler's own answer, so the two agree.
    add_compile_options(/w14062)
    # C4324 is "structure was padded due to alignment specifier". The cache-line aligned style records
    # (Rule, MediaQuery, ThemeToken, ThemePin) are exactly 64 bytes in Release and are padded only by
    # MSVC's larger Debug STL, where the padding is the intended effect of alignas, not a mistake.
    add_compile_options(/wd4324)
    # The Visual Studio generator compiles the files of one project serially unless /MP is set.
    # Ninja already parallelizes per file, so /MP there would only oversubscribe the CPU.
    if (CMAKE_GENERATOR MATCHES "Visual Studio")
        add_compile_options(/MP)
    endif()
    if (TESSERA_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
    if (TESSERA_WARN_PADDING)
        # C4820 is "padding added after member"; it is off by default at /W4.
        add_compile_options(/w14242)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if (TESSERA_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
    if (TESSERA_WARN_PADDING)
        add_compile_options(-Wpadded)
    endif()
endif()

# Sanitizer lanes (issue #702, docs/specs/memory-model-v1.md section 10). Included after the warning
# policy so a sanitizer build keeps TESSERA_WARNINGS_AS_ERRORS behaviour, and before any target is
# defined so the flags reach Tessera and the third-party libraries alike. Defaults to OFF, which adds
# no flag at all.
include(cmake/Sanitizers.cmake)

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
# Vector paths are on by default (ADR-057): the Linux lane already builds and proves this
# configuration, and the library's Icon and the chart track both need it. The Windows lane pins it
# OFF explicitly because the FetchContent download plus 37 translation units do not fit its build
# budget (#258), so the no-ThorVG configuration stays tested. A machine without network access can
# still configure with -DTESSERA_ENABLE_THORVG=OFF.
option(TESSERA_ENABLE_THORVG
    "Build the ThorVG-backed vector path rasterizer (D1, ADR-004, ADR-057)" ON)
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
#
# Wayland (issue #155) is the second, optional Linux window/presentation backend. It is detected
# with pkg-config (libwayland-client for the connection, wayland-protocols for the xdg-shell XML
# that wayland-scanner turns into client code) and stays strictly optional: a Linux box without the
# dev packages keeps the X11/stub build and prints a STATUS line.
set(TESSERA_X11_FOUND OFF)
set(TESSERA_WAYLAND_FOUND OFF)
if (UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN)
    find_package(X11 QUIET)
    if (X11_FOUND)
        set(TESSERA_X11_FOUND ON)
        message(STATUS "X11 found: Linux window and Vulkan Xlib surface enabled")
    else()
        message(STATUS "X11 not found: Linux window/presentation disabled (stub)")
    endif()

    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(TESSERA_WAYLAND_CLIENT QUIET wayland-client)
        pkg_check_modules(TESSERA_WAYLAND_PROTOCOLS QUIET wayland-protocols)
    endif()
    find_program(TESSERA_WAYLAND_SCANNER NAMES wayland-scanner)
    if (TESSERA_WAYLAND_CLIENT_FOUND AND TESSERA_WAYLAND_PROTOCOLS_FOUND
            AND TESSERA_WAYLAND_SCANNER)
        # The xdg-shell protocol XML ships with wayland-protocols; its absolute path is the build
        # input wayland-scanner consumes.
        pkg_get_variable(TESSERA_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        set(TESSERA_XDG_SHELL_XML
            "${TESSERA_WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
        if (EXISTS "${TESSERA_XDG_SHELL_XML}")
            set(TESSERA_WAYLAND_FOUND ON)
            message(STATUS
                "Wayland found: Linux Wayland window and Vulkan Wayland surface enabled "
                "(${TESSERA_WAYLAND_SCANNER}, ${TESSERA_XDG_SHELL_XML})")
        else()
            message(STATUS
                "Wayland xdg-shell XML not found: Linux Wayland support disabled "
                "(looked for ${TESSERA_XDG_SHELL_XML})")
        endif()
    else()
        message(STATUS "Wayland not found: Linux Wayland window/presentation disabled")
    endif()
endif()

# Wayland protocol codegen (issue #155): wayland-scanner turns wayland-protocols' xdg-shell XML into
# the client code src/platform/linux/window_wayland.cpp links against.
include(cmake/TesseraWayland.cmake)

add_subdirectory(third_party)
add_subdirectory(src/rendering/rhi)
add_subdirectory(src/rendering/memory)
add_subdirectory(src/rendering/gpu)
add_subdirectory(src/rendering/damage)
add_subdirectory(src/rendering/golden)
add_subdirectory(src/rendering/null)
add_subdirectory(src/rendering/path)
add_subdirectory(src/rendering/image)
add_subdirectory(src/rendering/software)
add_subdirectory(src/diagnostics)
# The in-tree D-Bus client: the wire codec, SASL and transport seam. Bottom-level on purpose, so
# both the AT-SPI2 accessibility bridge (src/ui/accessibility) and the HAL's Linux appearance
# watcher (src/platform/linux, `org.freedesktop.portal.Settings`) can link it without the HAL
# depending on the UI layer.
add_subdirectory(src/dbus)
add_subdirectory(src/hal/timer)
add_subdirectory(src/hal/window)
# AF7 (issue #179): the filesystem seam the storage module writes through. It lives beside the
# timer and window halves rather than under `src/appfw/`, so a later module can use it without
# depending on the app framework.
add_subdirectory(src/hal/filesystem)
# AF6 (issue #178): the borrowed native-surface view. It sits beside the other hal halves so a
# renderer can take a surface without depending on the window module that owns the handles.
add_subdirectory(src/hal/surface)
add_subdirectory(src/runtime)
add_subdirectory(src/ui/input)
# The browser HAL (ADR-046, issue #290): pure translation/scheduling everywhere, exports Emscripten only.
add_subdirectory(src/platform/web)
add_subdirectory(src/ui/scene)
add_subdirectory(src/ui/behavior)
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
# Window size classes (issue #692): the compact/medium/expanded buckets `useBreakpoint()` returns.
add_subdirectory(src/ui/responsive)
add_subdirectory(src/ui/capsule)
# The dev-mode compiler-error overlay (issue #78): a consumer of the `compiler-diagnostics-v1`
# document that renders into the app's existing OverlayHost. Dev-only (`TESSERA_ENABLE_DEVTOOLS`
# will gate the app-side mount); the library itself builds unconditionally so its tests always run.
add_subdirectory(src/ui/devtools)
add_subdirectory(src/app)
# ADR-050 opt-in app framework (AF1, issue #173): the service layer sits beside `src/app` and is not
# part of it, so an app that does not adopt the framework links none of it.
add_subdirectory(src/appfw)
# App-level test support (issue #454): the deterministic stack an app's own test drives - the manual
# clock and queues, the service fakes, the synthetic OS events and the `tap`/`type_text`/`advance`
# verbs. No shipping target links it; it is a static library so that separation is a build-graph fact.
add_subdirectory(src/testing)
add_subdirectory(src/harness)
add_subdirectory(src/schema)
add_subdirectory(src/compiler/analyzer)
add_subdirectory(src/compiler/ir)
add_subdirectory(src/compiler/eval)
add_subdirectory(src/compiler/ids)
add_subdirectory(src/compiler/services)
add_subdirectory(src/compiler/rules)
add_subdirectory(src/compiler/lowering)
add_subdirectory(src/compiler/codegen)
# CSS modules (issue #463): the scan that scopes a `.module.css` file's classes and keyframes. It
# links the scene-free tokenizer only, so it sits beside codegen rather than under `src/ui`.
add_subdirectory(src/compiler/css_module)
add_subdirectory(src/compiler/format)
add_subdirectory(tools/schema_dump)
# Front-end corpus measurement (spike #660, ADR-066): runs the parser and the Tier-1 rules over a
# corpus of React/TSX files and reports the frequency of each rejection. A measurement tool, not a
# shipped artifact; it links the front end and the rules only.
add_subdirectory(tools/corpus)
# `tessera icons add` (ADR-047 item 8): build-time Iconify import with the license gate.
add_subdirectory(tools/icons)
# `tessera-mcp` (issue #172): the stdio adapter a design-time client drives. It links the harness and
# nothing under `src/compiler` or `src/harness` learns about MCP, so the protocol stays at the edge.
add_subdirectory(tools/mcp)
# `tessera port-check` (ADR-058 section 5 item 1, issue #546): reads a Vite project's package.json
# and classifies each dependency by how much of it ports. It links nothing - the class table and a
# narrow manifest reader - so it builds on every lane, including a cross-compile where nothing else
# in tools/ can run.
add_subdirectory(tools/port-check)
add_subdirectory(tools/fmt)

# docs/specs/shader-manifest-v1.md sections 1 and 5: the manifest is a repository build input and
# `tessera_schema` validates it before glslc. The checker target must exist before the Vulkan
# shader rules reference it, so it is defined here; the cross-compile rules (and their ctest
# checks) are added after enable_testing() below.
set(TESSERA_SHADER_MANIFEST "${PROJECT_SOURCE_DIR}/tools/shaders/manifest.json")
if (TESSERA_BUILD_VULKAN OR TESSERA_SHADER_CROSS)
    add_subdirectory(tools/shaders)

    # The checker runs at BUILD time, so it must be a host executable. A cross-compiling build
    # (Android, iOS) would otherwise produce a target binary the host cannot execute, and every
    # shader rule would fail with "'tessera_shader_manifest_check' is not recognized" - which is
    # exactly how the Android Vulkan build failed before this. Build it for the host in a separate
    # project (tools/shaders/host) and point the rules at that binary.
    if (CMAKE_CROSSCOMPILING)
        include(ExternalProject)
        # A relative BINARY_DIR keeps the pins gate able to resolve this declaration: it expands
        # ${PROJECT_SOURCE_DIR} but not ${PROJECT_BINARY_DIR}, so an absolute build path would read as
        # an unresolvable pin expression. Relative paths are resolved against the current binary dir.
        set(TESSERA_SHADER_MANIFEST_CHECK_HOST_DIR
            "host-tools/shader-manifest-check")
        # The host tool must be built with the HOST toolchain, so it cannot inherit this project's
        # compiler: a cross-compiling configure sets CMAKE_CXX_COMPILER to the NDK's clang, and
        # passing that through would build a target binary again. The toolchain file is cleared so no
        # target sysroot leaks in.
        #
        # The generator is chosen by what THIS CMake can actually run, in this order:
        #
        #   1. Visual Studio, when vswhere finds an installation AND this CMake knows that
        #      generator. The second half matters: the NDK's bundled CMake 3.31.6 knows VS 17 2022
        #      but NOT VS 18 2026, so a host with only VS 2026 installed fails with "Could not create
        #      named generator" even though vswhere reports it. A VS generator is also the only one
        #      that locates MSVC by itself, which matters under Gradle where no compiler is on PATH.
        #   2. Ninja, when it is on PATH and a compiler is too.
        #   3. Unix Makefiles, the last resort on a POSIX host.
        set(TESSERA_HOST_GENERATOR "")
        set(TESSERA_HOST_GENERATOR_PLATFORM "")
        # The host configure needs a cmake that knows the host's generator. The NDK bundles 3.31.6,
        # which predates VS 2026; a system cmake 4.x knows it. Prefer the system one when it exists.
        set(TESSERA_HOST_CMAKE "${CMAKE_COMMAND}")
        if (CMAKE_HOST_WIN32)
            find_program(TESSERA_SYSTEM_CMAKE
                NAMES cmake
                HINTS "$ENV{ProgramFiles}/CMake/bin"
                NO_DEFAULT_PATH)
            if (TESSERA_SYSTEM_CMAKE)
                set(TESSERA_HOST_CMAKE "${TESSERA_SYSTEM_CMAKE}")
            endif()
        endif()
        if (CMAKE_HOST_WIN32)
            find_program(TESSERA_VSWHERE
                NAMES vswhere
                HINTS "$ENV{ProgramFiles}/Microsoft Visual Studio/Installer"
                      "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer")
            if (TESSERA_VSWHERE)
                execute_process(
                    COMMAND "${TESSERA_VSWHERE}" -latest -property installationVersion
                    OUTPUT_VARIABLE TESSERA_VS_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
                # installationVersion is "18.12.3..." for VS 2026, "17.x" for VS 2022. Only offer a
                # generator this CMake knows: `cmake --help` lists them, and an unknown name fails
                # the ExternalProject configure.
                execute_process(
                    COMMAND "${TESSERA_HOST_CMAKE}" --help
                    OUTPUT_VARIABLE TESSERA_CMAKE_GENERATORS
                    ERROR_QUIET)
                if (TESSERA_VS_VERSION MATCHES "^18\\." AND
                    TESSERA_CMAKE_GENERATORS MATCHES "Visual Studio 18 2026")
                    set(TESSERA_HOST_GENERATOR "Visual Studio 18 2026")
                elseif (TESSERA_VS_VERSION MATCHES "^17\\." AND
                        TESSERA_CMAKE_GENERATORS MATCHES "Visual Studio 17 2022")
                    set(TESSERA_HOST_GENERATOR "Visual Studio 17 2022")
                endif()
                if (TESSERA_HOST_GENERATOR)
                    set(TESSERA_HOST_GENERATOR_PLATFORM x64)
                endif()
            endif()
        endif()
        if (NOT TESSERA_HOST_GENERATOR)
            find_program(TESSERA_HOST_NINJA NAMES ninja)
            if (TESSERA_HOST_NINJA)
                set(TESSERA_HOST_GENERATOR "Ninja")
            else()
                set(TESSERA_HOST_GENERATOR "Unix Makefiles")
            endif()
        endif()
        ExternalProject_Add(tessera_shader_manifest_check_host
            SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/shaders/host"
            BINARY_DIR "${TESSERA_SHADER_MANIFEST_CHECK_HOST_DIR}"
            # The host configure runs with the SYSTEM cmake, not this one. The NDK bundles CMake
            # 3.31.6, which does not know the "Visual Studio 18 2026" generator that a current VS
            # install needs; the system cmake (4.x) does. Falling back to ${CMAKE_COMMAND} keeps a
            # host whose cmake is the only one available working.
            CMAKE_COMMAND "${TESSERA_HOST_CMAKE}"
            CMAKE_GENERATOR "${TESSERA_HOST_GENERATOR}"
            CMAKE_GENERATOR_PLATFORM "${TESSERA_HOST_GENERATOR_PLATFORM}"
            CMAKE_ARGS
                "-DCMAKE_BUILD_TYPE=Release"
                "-DTESSERA_HOST_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                "-DCMAKE_TOOLCHAIN_FILE="
            # A multi-config host generator (Visual Studio) puts the binary in a per-config
            # subdirectory and defaults to Debug, so the config is pinned here. The path below
            # matches it. The build runs with the same cmake as the configure: the NDK's bundled one
            # does not know the VS 2026 generator.
            BUILD_COMMAND "${TESSERA_HOST_CMAKE}" --build <BINARY_DIR> --config Release
            BUILD_ALWAYS OFF
            INSTALL_COMMAND ""
            COMMENT "Building the host shader manifest checker")
        # The rules run the host binary and depend on the ExternalProject target that produces it.
        # `TESSERA_SHADER_MANIFEST_CHECK` is the command, `..._DEP` the build-order dependency: an
        # imported target carries no rule of its own, so naming it in DEPENDS would leave ninja with
        # "no known rule to make it". The command is absolute because a custom command runs in its
        # own working directory, where a relative path would not resolve.
        set(TESSERA_SHADER_MANIFEST_CHECK
            "${PROJECT_BINARY_DIR}/${TESSERA_SHADER_MANIFEST_CHECK_HOST_DIR}/Release/tessera_shader_manifest_check${CMAKE_EXECUTABLE_SUFFIX}")
        set(TESSERA_SHADER_MANIFEST_CHECK_DEP tessera_shader_manifest_check_host)
    else()
        set(TESSERA_SHADER_MANIFEST_CHECK tessera_shader_manifest_check)
        set(TESSERA_SHADER_MANIFEST_CHECK_DEP tessera_shader_manifest_check)
    endif()
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

# Metal mapping tables are API-free and always built (their unit tests run on every host); the
# Objective-C++ device half is gated inside (TESSERA_BUILD_METAL, Apple only). ADR-053.
add_subdirectory(src/rendering/backend/metal)

if (TESSERA_BUILD_VULKAN)
    add_subdirectory(src/rendering/backend/vulkan)
    # ADR-071 spike (issue #683): a GPU compute path rasterizer measured against ThorVG at 4K. Off by
    # default; it changes no default and is not registered with ctest.
    option(TESSERA_BUILD_COMPUTE_RASTER_SPIKE
        "Build the ADR-071 GPU compute path-rasterizer spike (a measurement tool, not in ctest)" OFF)
    if (TESSERA_BUILD_COMPUTE_RASTER_SPIKE)
        add_subdirectory(tools/poc/compute_raster)
    endif()
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
    # The Android app shell (issue #189): the native library Gradle packages into an APK. It is
    # built only for Android, and only when the window driver exists, because it links it.
    if (ANDROID AND TARGET tessera_app_window)
        add_subdirectory(android/app/src/main/cpp)
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
