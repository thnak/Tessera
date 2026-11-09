# SPDX-License-Identifier: MIT
#
# tessera_embed_assets(<target> MANIFEST <path> [NAMESPACE <ns>])
#
# Turns a small asset manifest into a generated C++ source/header pair and compiles it into
# <target>, so the asset bytes ship inside the statically linked binary (no runtime filesystem
# dependency). The manifest format is documented in docs/design/thorvg-spike/09-assets.md:
#
#   # comment
#   icons/app.svg = relative/or/absolute/path.svg
#   meta/readme.txt = readme.txt
#
# Asset paths are resolved relative to the manifest file. The generated files are deterministic
# (sorted, LF, no timestamps or absolute paths); re-running the same manifest produces identical
# bytes. Generation runs as a CMake script (`cmake/embed_assets.cmake`) on the BUILD HOST, so it
# works in cross-compiling builds (Android/iOS) where a target binary cannot be executed.

include_guard(GLOBAL)

function(tessera_embed_assets target)
    cmake_parse_arguments(ARG "" "MANIFEST;NAMESPACE;STEM" "" ${ARGN})
    if (NOT TARGET ${target})
        message(FATAL_ERROR "tessera_embed_assets: target '${target}' does not exist")
    endif()
    if (NOT ARG_MANIFEST)
        message(FATAL_ERROR "tessera_embed_assets(${target}): MANIFEST is required")
    endif()

    get_filename_component(_manifest "${ARG_MANIFEST}" ABSOLUTE)
    if (NOT EXISTS "${_manifest}")
        message(FATAL_ERROR "tessera_embed_assets(${target}): manifest not found: ${_manifest}")
    endif()
    get_filename_component(_manifest_dir "${_manifest}" DIRECTORY)

    # Track every asset file as a build dependency and fail early on a malformed manifest, so a
    # missing file is a configure error instead of a confusing generator failure.
    set(_asset_files "")
    file(STRINGS "${_manifest}" _manifest_lines)
    foreach(_line IN LISTS _manifest_lines)
        string(STRIP "${_line}" _line)
        if (_line STREQUAL "" OR _line MATCHES "^#")
            continue()
        endif()
        if (NOT _line MATCHES "^([^=]+)=(.+)$")
            message(FATAL_ERROR
                "tessera_embed_assets(${target}): manifest line is not 'name = path': ${_line}")
        endif()
        string(STRIP "${CMAKE_MATCH_2}" _asset_relative)
        if (_asset_relative STREQUAL "")
            message(FATAL_ERROR "tessera_embed_assets(${target}): empty asset path")
        endif()
        get_filename_component(_asset "${_asset_relative}" ABSOLUTE BASE_DIR "${_manifest_dir}")
        if (NOT EXISTS "${_asset}")
            message(FATAL_ERROR
                "tessera_embed_assets(${target}): asset file not found: ${_asset}")
        endif()
        list(APPEND _asset_files "${_asset}")
    endforeach()

    set(_namespace "${ARG_NAMESPACE}")
    if (_namespace STREQUAL "")
        set(_namespace "tessera::ui::assets::${target}")
    endif()

    set(_stem "${target}")
    if (ARG_STEM)
        set(_stem "${ARG_STEM}")
    endif()
    set(_output_dir "${CMAKE_CURRENT_BINARY_DIR}/tessera_assets_${_stem}")
    set(_header "${_output_dir}/${_stem}_assets.hpp")
    set(_source "${_output_dir}/${_stem}_assets.cpp")

    # The generation logic is a CMake script, not a target-built executable: asset embedding must
    # work in cross-compiling builds (Android/iOS), where a target binary cannot run on the host.
    # ${CMAKE_COMMAND} always exists and runs on the build host.
    set(_embed_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/embed_assets.cmake")

    add_custom_command(
        OUTPUT "${_header}" "${_source}"
        COMMAND ${CMAKE_COMMAND}
            "-DMANIFEST=${_manifest}"
            "-DNAMESPACE=${_namespace}"
            "-DHEADER=${_header}"
            "-DSOURCE=${_source}"
            -P "${_embed_script}"
        DEPENDS "${_embed_script}" "${_manifest}" ${_asset_files}
        COMMENT "Embedding ${target} assets"
        VERBATIM)

    target_sources(${target} PRIVATE "${_source}")
    target_include_directories(${target} PRIVATE "${_output_dir}")
endfunction()
