# SPDX-License-Identifier: MIT
#
# ThorVG vector rasterizer bring-up (decision D1, ADR-004).
#
# Upstream ThorVG v1.1.3 is meson-only: the pinned release archive contains no CMakeLists.txt
# anywhere, so FetchContent_MakeAvailable() only downloads and extracts it. This wrapper compiles
# the pinned source set directly with the smallest feature set the PathRasterizer seam needs (CPU
# engine, SVG/raw loaders, file I/O, partial render). An ExternalProject + meson build would add a
# build-time tool dependency without changing the produced artifact, so it is deliberately not
# used. The archive hash below is the reproducibility pin; re-pin both the URL and the hash
# together when the dependency is upgraded.

include(FetchContent)

function(tessera_add_thorvg)
    set(TESSERA_THORVG_VERSION "1.1.3")
    set(TESSERA_THORVG_URL
        "https://github.com/thorvg/thorvg/archive/refs/tags/v1.1.3.tar.gz")
    set(TESSERA_THORVG_SHA256
        "ba9957aeeadb4151d4486f11c638ae118c00210fa4906daa7b30143aa341e690")

    FetchContent_Declare(thorvg
        URL ${TESSERA_THORVG_URL}
        URL_HASH SHA256=${TESSERA_THORVG_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

    # No CMakeLists.txt upstream: this populates the source tree and takes no other action.
    FetchContent_MakeAvailable(thorvg)

    if (NOT thorvg_SOURCE_DIR)
        message(FATAL_ERROR
            "ThorVG v${TESSERA_THORVG_VERSION} was not populated; check the pinned archive")
    endif()

    # Meson generates this header from its options. Keep exactly the features the seam uses; every
    # other THORVG_* feature stays undefined (and therefore off). ThorVG 1.1.3 exposes CPU, GL and
    # WebGPU engines only - there is no Vulkan engine, so V1 rasterizes on the CPU and uploads the
    # buffer through the Tessera RHI.
    set(_tvg_generated "${CMAKE_CURRENT_BINARY_DIR}/thorvg-generated")
    file(MAKE_DIRECTORY "${_tvg_generated}")
    file(WRITE "${_tvg_generated}/config.h"
"#define THORVG_VERSION_STRING \"1.1.3\"
#define THORVG_CPU_ENGINE_SUPPORT 1
#define THORVG_SVG_LOADER_SUPPORT 1
#define THORVG_PARTIAL_RENDER_SUPPORT 1
#define THORVG_FILE_IO_SUPPORT 1
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN 1
#endif
")

    # Pinned tree, so a plain glob is deterministic. src/renderer/gpu_engine holds the GL and
    # WebGPU backends, which this build does not enable.
    file(GLOB_RECURSE _tvg_common "${thorvg_SOURCE_DIR}/src/common/*.cpp")
    file(GLOB_RECURSE _tvg_renderer "${thorvg_SOURCE_DIR}/src/renderer/*.cpp")
    list(FILTER _tvg_renderer EXCLUDE REGEX "/gpu_engine/")
    file(GLOB_RECURSE _tvg_svg "${thorvg_SOURCE_DIR}/src/loaders/svg/*.cpp")
    file(GLOB_RECURSE _tvg_raw "${thorvg_SOURCE_DIR}/src/loaders/raw/*.cpp")

    add_library(tessera_thorvg STATIC
        ${_tvg_common}
        ${_tvg_renderer}
        ${_tvg_svg}
        ${_tvg_raw})

    target_include_directories(tessera_thorvg PUBLIC
        $<BUILD_INTERFACE:${thorvg_SOURCE_DIR}/inc>
        $<BUILD_INTERFACE:${_tvg_generated}>
        $<BUILD_INTERFACE:${thorvg_SOURCE_DIR}/src/common>
        $<BUILD_INTERFACE:${thorvg_SOURCE_DIR}/src/renderer>
        $<BUILD_INTERFACE:${thorvg_SOURCE_DIR}/src/renderer/cpu_engine>
        $<BUILD_INTERFACE:${thorvg_SOURCE_DIR}/src/loaders/raw>
        $<BUILD_INTERFACE:${thorvg_SOURCE_DIR}/src/loaders/svg>)

    # TVG_STATIC selects the non-DLL import/export path in <thorvg.h>; static linking is a project
    # requirement (ADR-012).
    target_compile_definitions(tessera_thorvg PUBLIC TVG_STATIC)
    if (WIN32)
        target_compile_definitions(tessera_thorvg PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    endif()

    # ThorVG is C++14-era code; build it as C++17 like upstream's meson default rather than with
    # the project's C++23 baseline.
    set_target_properties(tessera_thorvg PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    # Third-party sources are not held to this project's warning policy; Tessera code keeps
    # -Werror.
    if (MSVC)
        target_compile_options(tessera_thorvg PRIVATE /W0)
    else()
        target_compile_options(tessera_thorvg PRIVATE -w)
    endif()

    get_target_property(_tvg_source_count tessera_thorvg SOURCES)
    list(LENGTH _tvg_source_count _tvg_source_count)
    message(STATUS
        "ThorVG v${TESSERA_THORVG_VERSION} (meson-only upstream, CMake wrapper): "
        "${_tvg_source_count} sources, CPU engine")
endfunction()
