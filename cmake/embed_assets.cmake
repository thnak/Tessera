# SPDX-License-Identifier: MIT
#
# Host-independent embedded-asset generation. This script is run at build time by
# cmake/TesseraAssets.cmake as:
#
#   cmake -DMANIFEST=<manifest> -DNAMESPACE=<ns> -DHEADER=<file.hpp> -DSOURCE=<file.cpp>
#         -P cmake/embed_assets.cmake
#
# It exists instead of a target-built generator executable because asset generation must run on the
# HOST in cross-compiling builds (Android NDK, iOS SDK): executing a target binary during the build
# fails with "not found"/Exec format errors. CMake is always available and host-independent.
#
# Output is deterministic and matches the previous C++ generator's shape: entries sorted by
# normalized name, one packed byte array in that order, lowercase fixed-width hex, LF endings, only
# the manifest basename in comments (no timestamps, no absolute paths, no locale-dependent output).
# The manifest format is documented in docs/design/thorvg-spike/09-assets.md.

cmake_minimum_required(VERSION 3.24)

foreach(required MANIFEST HEADER SOURCE NAMESPACE)
    if (NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "embed_assets: -D${required}=... is required")
    endif()
endforeach()

get_filename_component(_manifest "${MANIFEST}" ABSOLUTE)
if (NOT EXISTS "${_manifest}")
    message(FATAL_ERROR "embed_assets: cannot open manifest")
endif()
get_filename_component(_manifest_dir "${_manifest}" DIRECTORY)
get_filename_component(_manifest_name "${_manifest}" NAME)
get_filename_component(_header "${HEADER}" ABSOLUTE)
get_filename_component(_source "${SOURCE}" ABSOLUTE)

if (NOT NAMESPACE MATCHES "^[A-Za-z_][A-Za-z0-9_:]*$")
    message(FATAL_ERROR "embed_assets: invalid namespace")
endif()

get_filename_component(_header_dir "${_header}" DIRECTORY)
get_filename_component(_source_dir "${_source}" DIRECTORY)
if (NOT _header_dir STREQUAL _source_dir)
    message(FATAL_ERROR "embed_assets: HEADER and SOURCE must be in the same directory")
endif()
file(MAKE_DIRECTORY "${_header_dir}")
get_filename_component(_header_name "${_header}" NAME)

# Normalization rule shared with the runtime lookup (asset_bundle.hpp): '/' separators only, one
# optional leading '/' stripped, no empty/"."/".." segments, allowed charset, case-sensitive.
function(embed_normalize_name raw out_var)
    set(_name "${raw}")
    if (_name MATCHES "^/")
        string(SUBSTRING "${_name}" 1 -1 _name)
    endif()
    if (_name STREQUAL "")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    if (NOT _name MATCHES "^[A-Za-z0-9._/-]+$")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()
    string(REPLACE "/" ";" _segments "${_name}")
    foreach(_segment IN LISTS _segments)
        if (_segment STREQUAL "" OR _segment STREQUAL "." OR _segment STREQUAL "..")
            set(${out_var} "" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "${_name}" PARENT_SCOPE)
endfunction()

function(embed_mime_for path out_var)
    get_filename_component(_extension "${path}" EXT)
    string(TOLOWER "${_extension}" _extension)
    if (_extension STREQUAL ".svg")
        set(${out_var} "image/svg+xml" PARENT_SCOPE)
    elseif (_extension STREQUAL ".txt")
        set(${out_var} "text/plain" PARENT_SCOPE)
    elseif (_extension STREQUAL ".json")
        set(${out_var} "application/json" PARENT_SCOPE)
    elseif (_extension STREQUAL ".png")
        set(${out_var} "image/png" PARENT_SCOPE)
    elseif (_extension STREQUAL ".jpg" OR _extension STREQUAL ".jpeg")
        set(${out_var} "image/jpeg" PARENT_SCOPE)
    elseif (_extension STREQUAL ".webp")
        set(${out_var} "image/webp" PARENT_SCOPE)
    else()
        set(${out_var} "application/octet-stream" PARENT_SCOPE)
    endif()
endfunction()

file(STRINGS "${_manifest}" _manifest_lines)
set(_names "")
set(_hexes "")
set(_sizes "")
set(_mimes "")
set(_line_number 0)
foreach(_line IN LISTS _manifest_lines)
    math(EXPR _line_number "${_line_number} + 1")
    string(STRIP "${_line}" _line)
    if (_line STREQUAL "" OR _line MATCHES "^#")
        continue()
    endif()
    if (NOT _line MATCHES "^([^=]+)=(.+)$")
        message(FATAL_ERROR
            "embed_assets: manifest line ${_line_number} is not 'name = path'")
    endif()
    string(STRIP "${CMAKE_MATCH_1}" _raw_name)
    string(STRIP "${CMAKE_MATCH_2}" _raw_path)
    if (_raw_name STREQUAL "" OR _raw_path STREQUAL "")
        message(FATAL_ERROR
            "embed_assets: manifest line ${_line_number} has an empty name or path")
    endif()
    embed_normalize_name("${_raw_name}" _name)
    if (_name STREQUAL "")
        message(FATAL_ERROR
            "embed_assets: manifest line ${_line_number} has an invalid asset name")
    endif()
    get_filename_component(_asset_path "${_raw_path}" ABSOLUTE BASE_DIR "${_manifest_dir}")
    if (NOT EXISTS "${_asset_path}")
        message(FATAL_ERROR
            "embed_assets: cannot open asset file: ${_raw_path}")
    endif()
    file(READ "${_asset_path}" _hex HEX)
    string(LENGTH "${_hex}" _hex_length)
    math(EXPR _size "${_hex_length} / 2")
    embed_mime_for("${_asset_path}" _mime)
    list(APPEND _names "${_name}")
    list(APPEND _hexes "${_hex}")
    list(APPEND _sizes "${_size}")
    list(APPEND _mimes "${_mime}")
endforeach()

# Sort by normalized name; carry the source index so the byte lists follow the same order.
list(LENGTH _names _asset_count)
set(_pairs "")
set(_index 0)
while(_index LESS _asset_count)
    list(GET _names ${_index} _name)
    list(APPEND _pairs "${_name}|${_index}")
    math(EXPR _index "${_index} + 1")
endwhile()
list(SORT _pairs)

set(_sorted_names "")
set(_sorted_hexes "")
set(_sorted_sizes "")
set(_sorted_mimes "")
set(_previous_name "")
foreach(_pair IN LISTS _pairs)
    string(FIND "${_pair}" "|" _separator)
    string(SUBSTRING "${_pair}" 0 ${_separator} _name)
    math(EXPR _name_length "${_separator} + 1")
    string(SUBSTRING "${_pair}" ${_name_length} -1 _source_index)
    if (_previous_name STREQUAL _name)
        message(FATAL_ERROR "embed_assets: duplicate asset name '${_name}'")
    endif()
    set(_previous_name "${_name}")
    list(GET _hexes ${_source_index} _hex)
    list(GET _sizes ${_source_index} _size)
    list(GET _mimes ${_source_index} _mime)
    list(APPEND _sorted_names "${_name}")
    list(APPEND _sorted_hexes "${_hex}")
    list(APPEND _sorted_sizes "${_size}")
    list(APPEND _sorted_mimes "${_mime}")
endforeach()

set(_all_hex "")
set(_total_bytes 0)
foreach(_hex IN LISTS _sorted_hexes)
    string(APPEND _all_hex "${_hex}")
endforeach()
foreach(_size IN LISTS _sorted_sizes)
    math(EXPR _total_bytes "${_total_bytes} + ${_size}")
endforeach()

# Byte array: 12 bytes per line, "0xNN," items separated by one space, exactly the previous shape.
string(LENGTH "${_all_hex}" _all_hex_length)
if (_all_hex_length EQUAL 0)
    set(_data_block "constexpr unsigned char kAssetData[] = {0x00};\n")
else()
    set(_bytes_text "")
    math(EXPR _byte_count "${_all_hex_length} / 2")
    math(EXPR _line_count "(${_byte_count} + 11) / 12")
    set(_line 0)
    while(_line LESS _line_count)
        math(EXPR _start "${_line} * 24")
        math(EXPR _chunk_length "${_all_hex_length} - ${_start}")
        if (_chunk_length GREATER 24)
            set(_chunk_length 24)
        endif()
        string(SUBSTRING "${_all_hex}" ${_start} ${_chunk_length} _chunk)
        string(REGEX REPLACE "(..)" "0x\\1, " _line_text "${_chunk}")
        string(REGEX REPLACE " $" "" _line_text "${_line_text}")
        string(APPEND _bytes_text "\n    ${_line_text}")
        math(EXPR _line "${_line} + 1")
    endwhile()
    string(APPEND _bytes_text "\n")
    set(_data_block "constexpr unsigned char kAssetData[] = {${_bytes_text}};\n")
endif()

set(_entries_block "constexpr ::tessera::ui::assets::AssetEntry kAssetEntries[] = {\n")
if (_asset_count EQUAL 0)
    string(APPEND _entries_block "    {},\n")
else()
    set(_offset 0)
    set(_entry_index 0)
    while(_entry_index LESS _asset_count)
        list(GET _sorted_names ${_entry_index} _name)
        list(GET _sorted_sizes ${_entry_index} _size)
        list(GET _sorted_mimes ${_entry_index} _mime)
        string(APPEND _entries_block
            "    {\"${_name}\", ${_offset}, ${_size}, \"${_mime}\"},\n")
        math(EXPR _offset "${_offset} + ${_size}")
        math(EXPR _entry_index "${_entry_index} + 1")
    endwhile()
endif()
string(APPEND _entries_block "};\n")

set(_header_text "// SPDX-License-Identifier: MIT\n")
string(APPEND _header_text
    "// Generated by cmake/embed_assets.cmake from ${_manifest_name}; do not edit.\n")
string(APPEND _header_text
    "// Regenerate with: cmake -DMANIFEST=<manifest> -DNAMESPACE=${NAMESPACE}\n")
string(APPEND _header_text
    "//                  -DHEADER=<file.hpp> -DSOURCE=<file.cpp> -P cmake/embed_assets.cmake\n\n")
string(APPEND _header_text "#pragma once\n\n")
string(APPEND _header_text "#include \"tessera/ui/assets/asset_bundle.hpp\"\n\n")
string(APPEND _header_text "#include <cstddef>\n\n")
string(APPEND _header_text "namespace ${NAMESPACE} {\n\n")
string(APPEND _header_text "// Number of embedded assets and total payload bytes.\n")
string(APPEND _header_text
    "inline constexpr std::size_t kAssetCount = ${_asset_count};\n")
string(APPEND _header_text
    "inline constexpr std::size_t kAssetBytes = ${_total_bytes};\n\n")
string(APPEND _header_text
    "[[nodiscard]] ::tessera::ui::assets::AssetBundle bundle() noexcept;\n\n")
string(APPEND _header_text "} // namespace ${NAMESPACE}\n")

set(_source_text "// SPDX-License-Identifier: MIT\n")
string(APPEND _source_text
    "// Generated by cmake/embed_assets.cmake from ${_manifest_name}; do not edit.\n\n")
string(APPEND _source_text "#include \"${_header_name}\"\n\n")
string(APPEND _source_text "#include <cstddef>\n")
string(APPEND _source_text "#include <span>\n\n")
string(APPEND _source_text "namespace ${NAMESPACE} {\nnamespace {\n\n")
string(APPEND _source_text "${_data_block}\n")
string(APPEND _source_text "${_entries_block}\n")
string(APPEND _source_text "} // namespace\n\n")
string(APPEND _source_text
    "::tessera::ui::assets::AssetBundle bundle() noexcept {\n")
string(APPEND _source_text
    "    return ::tessera::ui::assets::AssetBundle(\n")
string(APPEND _source_text
    "        std::span<const ::tessera::ui::assets::AssetEntry>(kAssetEntries, kAssetCount),\n")
string(APPEND _source_text
    "        std::as_bytes(std::span<const unsigned char>(kAssetData, kAssetBytes)));\n")
string(APPEND _source_text "}\n\n")
string(APPEND _source_text "} // namespace ${NAMESPACE}\n")

# Deterministic LF output on every host. `file(WRITE)` uses the host's newline convention on
# Windows, so write a temporary and let configure_file with NEWLINE_STYLE UNIX normalize it. The
# generated content never contains '@', so @ONLY substitution is a no-op.
function(embed_write_lf path content)
    set(_temporary "${path}.embed-tmp")
    file(WRITE "${_temporary}" "${content}")
    configure_file("${_temporary}" "${path}" @ONLY NEWLINE_STYLE UNIX)
    file(REMOVE "${_temporary}")
endfunction()

embed_write_lf("${_header}" "${_header_text}")
embed_write_lf("${_source}" "${_source_text}")
message(STATUS
    "embed_assets: ${_asset_count} assets, ${_total_bytes} bytes -> ${_header_name}")
