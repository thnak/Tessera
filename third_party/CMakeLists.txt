#SPDX - License - Identifier : MIT

add_library(tessera_stb STATIC
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
