#SPDX - License - Identifier : MIT

add_library(tessera_stb STATIC
    stb_impl/stb_image_impl.cpp
    stb_impl/stb_image_write_impl.cpp)

target_include_directories(tessera_stb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/stb>)

if (MSVC)
    target_compile_definitions(tessera_stb PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(tessera_stb PRIVATE -w)
endif()

file(GLOB_RECURSE YOGA_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/yoga/yoga/*.cpp)

add_library(tessera_yoga STATIC ${YOGA_SOURCES})

target_include_directories(tessera_yoga PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/yoga)

target_compile_features(tessera_yoga PUBLIC cxx_std_20)

if (ANDROID)
    # Yoga's Android logger calls __android_log_vprint.
    target_link_libraries(tessera_yoga PUBLIC log)
endif()
target_compile_definitions(tessera_yoga PRIVATE
    $<$<CONFIG:Debug>:DEBUG>)

if (MSVC)
    target_compile_options(tessera_yoga PRIVATE /W0)
else()
    target_compile_options(tessera_yoga PRIVATE -w)
endif()

# FreeType backs font loading and glyph metrics in the text stack (ADR-006). The source tree is
# large, so it is pinned by archive hash and fetched at configure time instead of vendored; the
# image codecs are not needed inside a UI framework and stay disabled.
include(FetchContent)

set(FT_DISABLE_ZLIB TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)

FetchContent_Declare(
    freetype
    URL https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz
    URL_HASH SHA256=bc5c898e4756d373e0d991bab053036c5eb2aa7c0d5c67e8662ddc6da40c4103
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(freetype)

# Third-party sources are not held to this project's warning policy.
if (TARGET freetype)
    if (MSVC)
        target_compile_options(freetype PRIVATE /W0)
    else()
        target_compile_options(freetype PRIVATE -w)
    endif()

    # Stable module-facing name; Tessera code never includes FreeType headers.
    add_library(tessera_freetype INTERFACE)
    target_link_libraries(tessera_freetype INTERFACE freetype)
endif()

# HarfBuzz turns text runs into glyph ids/positions. It parses the same pinned font bytes as
# FreeType, so the FreeType integration and the platform shapers stay out of the build.
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GOBJECT OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GRAPHITE2 OFF CACHE BOOL "" FORCE)
set(HB_HAVE_CORETEXT OFF CACHE BOOL "" FORCE)
set(HB_HAVE_DIRECTWRITE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_UNISCRIBE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GDI OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    harfbuzz
    URL https://github.com/harfbuzz/harfbuzz/archive/refs/tags/10.2.0.tar.gz
    URL_HASH SHA256=11749926914fd488e08e744538f19329332487a6243eec39ef3c63efa154a578
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(harfbuzz)

if (TARGET harfbuzz)
    if (MSVC)
        target_compile_options(harfbuzz PRIVATE /W0)
    else()
        target_compile_options(harfbuzz PRIVATE -w)
    endif()

    add_library(tessera_harfbuzz INTERFACE)
    target_link_libraries(tessera_harfbuzz INTERFACE harfbuzz)
endif()

# ThorVG rasterizes vector paths behind the PathRasterizer seam (ADR-004, decision D1). It is
# opt-in because it is the largest optional dependency; the default build is unchanged when the
# option is OFF. Upstream is meson-only, so cmake/ThorVG.cmake builds the pinned sources.
if (TESSERA_ENABLE_THORVG)
    include(${PROJECT_SOURCE_DIR}/cmake/ThorVG.cmake)
    tessera_add_thorvg()
endif()
