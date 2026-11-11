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

# libjpeg-turbo gives JPEG a scaled decode (1/2, 1/4, 1/8 in the IDCT), so a thumbnail of a large
# photo never allocates the full raster (issue #14). Upstream refuses add_subdirectory(), so the
# archive is fetched without adding it and the plain-C library is built here: no SIMD (no assembler
# on any CI platform, identical output everywhere), no TurboJPEG API, no tools. 12- and 16-bit
# object sets are required by the library's precision-generic entry points, as upstream does.
FetchContent_Declare(
    libjpeg_turbo
    URL https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.0.4.tar.gz
    URL_HASH SHA256=0270f9496ad6d69e743f1e7b9e3e9398f5b4d606b6a47744df4b73df50f62e38
    SOURCE_SUBDIR tessera_not_a_subdirectory
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(libjpeg_turbo)

set(LJT ${libjpeg_turbo_SOURCE_DIR})
set(LJT_GEN ${CMAKE_CURRENT_BINARY_DIR}/libjpeg_turbo_config)
set(JPEG_LIB_VERSION 62)
set(VERSION 3.0.4)
set(LIBJPEG_TURBO_VERSION_NUMBER 3000004)
set(BUILD "tessera")
set(HIDDEN "")
set(SIZE_T ${CMAKE_SIZEOF_VOID_P})
if (MSVC)
    set(INLINE "__forceinline")
    set(THREAD_LOCAL "__declspec(thread)")
else()
    set(INLINE "inline __attribute__((always_inline))")
    set(THREAD_LOCAL "__thread")
endif()
set(D_ARITH_CODING_SUPPORTED 1)
configure_file(${LJT}/jconfig.h.in ${LJT_GEN}/jconfig.h)
configure_file(${LJT}/jconfigint.h.in ${LJT_GEN}/jconfigint.h)
configure_file(${LJT}/jversion.h.in ${LJT_GEN}/jversion.h)

set(LJT16_SOURCES jcapistd.c jccolor.c jcdiffct.c jclossls.c jcmainct.c jcprepct.c jcsample.c
    jdapistd.c jdcolor.c jddiffct.c jdlossls.c jdmainct.c jdpostct.c jdsample.c jutils.c)
set(LJT12_SOURCES ${LJT16_SOURCES} jccoefct.c jcdctmgr.c jdcoefct.c jddctmgr.c jdmerge.c
    jfdctfst.c jfdctint.c jidctflt.c jidctfst.c jidctint.c jidctred.c jquant1.c jquant2.c)
set(LJT_SOURCES ${LJT12_SOURCES} jcapimin.c jchuff.c jcicc.c jcinit.c jclhuff.c jcmarker.c
    jcmaster.c jcomapi.c jcparam.c jcphuff.c jctrans.c jdapimin.c jdatadst.c jdatasrc.c
    jdhuff.c jdicc.c jdinput.c jdlhuff.c jdmarker.c jdmaster.c jdphuff.c jdtrans.c jerror.c
    jfdctflt.c jmemmgr.c jmemnobs.c jpeg_nbits.c jaricom.c jdarith.c)

function(tessera_ljt_sources out)
    set(list "")
    foreach (name ${ARGN})
        list(APPEND list ${LJT}/${name})
    endforeach()
    set(${out} ${list} PARENT_SCOPE)
endfunction()

tessera_ljt_sources(LJT16_PATHS ${LJT16_SOURCES})
tessera_ljt_sources(LJT12_PATHS ${LJT12_SOURCES})
tessera_ljt_sources(LJT_PATHS ${LJT_SOURCES})

add_library(tessera_jpeg12 OBJECT ${LJT12_PATHS})
target_compile_definitions(tessera_jpeg12 PRIVATE BITS_IN_JSAMPLE=12)
add_library(tessera_jpeg16 OBJECT ${LJT16_PATHS})
target_compile_definitions(tessera_jpeg16 PRIVATE BITS_IN_JSAMPLE=16)
add_library(tessera_jpeg STATIC ${LJT_PATHS} $<TARGET_OBJECTS:tessera_jpeg12>
    $<TARGET_OBJECTS:tessera_jpeg16>)
foreach (jpeg_target tessera_jpeg12 tessera_jpeg16 tessera_jpeg)
    # Generated config first so it wins over any installed libjpeg header.
    target_include_directories(${jpeg_target} PRIVATE ${LJT_GEN} ${LJT})
    set_target_properties(${jpeg_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if (MSVC)
        target_compile_options(${jpeg_target} PRIVATE /W0)
        target_compile_definitions(${jpeg_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${jpeg_target} PRIVATE -w)
    endif()
endforeach()
target_include_directories(tessera_jpeg SYSTEM PUBLIC
    $<BUILD_INTERFACE:${LJT_GEN}>
    $<BUILD_INTERFACE:${LJT}>)

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

# SheenBidi resolves Unicode bidirectional embedding levels and per-line visual runs (Apache-2.0,
# pure C, upstream CMake, no meson). Pinned by archive hash; unity build keeps it a single object
# and disables its own non-unity test suite, so it is a small static dependency.
set(SB_CONFIG_UNITY ON CACHE BOOL "" FORCE)
set(SB_CONFIG_EXPERIMENTAL_TEXT_API OFF CACHE BOOL "" FORCE)
set(BUILD_GENERATOR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    sheenbidi
    URL https://github.com/Tehreer/SheenBidi/archive/refs/tags/v3.0.0.tar.gz
    URL_HASH SHA256=86c56014034739ba39a24c23eb00323b0bf6f737354f665786015fca842af786
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sheenbidi)

if (TARGET SheenBidi)
    if (MSVC)
        target_compile_options(SheenBidi PRIVATE /W0)
    else()
        target_compile_options(SheenBidi PRIVATE -w)
    endif()

    add_library(tessera_sheenbidi INTERFACE)
    target_link_libraries(tessera_sheenbidi INTERFACE SheenBidi)
endif()

# ThorVG rasterizes vector paths behind the PathRasterizer seam (ADR-004, decision D1). It is
# opt-in because it is the largest optional dependency; the default build is unchanged when the
# option is OFF. Upstream is meson-only, so cmake/ThorVG.cmake builds the pinned sources.
if (TESSERA_ENABLE_THORVG)
    include(${PROJECT_SOURCE_DIR}/cmake/ThorVG.cmake)
    tessera_add_thorvg()
endif()

# ICU (static build with reduced data, docs/design/icu-data.md). The whole dependency - static
# common/i18n libraries, the trimmed and linked data blob, and the locale manifest - is built by
# third_party/icu/CMakeLists.txt and is opt-in: OFF keeps the default build, the presets, and CI
# unchanged. Move the option block below to the root options block when ICU becomes mandatory.
option(TESSERA_ICU_REDUCED_DATA
    "Build static ICU with the reduced data blob and the documented locale list" OFF)
set(TESSERA_ICU_TOOLS_DIR "" CACHE PATH
    "Pinned ICU bin directory containing icupkg and genccode")
set(TESSERA_ICU_FULL_DATA "" CACHE FILEPATH
    "Full upstream ICU data package to trim, icudt<version><type>.dat")
set(TESSERA_ICU_SOURCE_DIR "" CACHE PATH
    "Pre-fetched ICU source tree (empty fetches the pinned release archive)")

if (TESSERA_ICU_REDUCED_DATA)
    add_subdirectory(icu)
endif()
