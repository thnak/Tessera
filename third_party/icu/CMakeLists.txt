# SPDX-License-Identifier: MIT
#
# Spike task-94: reduced ICU static data (measured cost, see docs/design/icu-data.md).
#
# ICU is not vendored or fetched by this repository yet (docs/specs/dependencies.md lists it as
# "V1 hard" with no build integration), so this step cannot build ICU. It starts from a pinned
# upstream data package and a pinned upstream `icupkg`, both supplied through cache variables,
# and produces the reduced blob a static-data ICU build would embed.
#
# Everything here is gated on TESSERA_ICU_REDUCED_DATA, which is declared in
# third_party/CMakeLists.txt and defaults to OFF, so the default build, the presets, and CI are
# unchanged.

if(NOT TESSERA_ICU_REDUCED_DATA)
    return()
endif()

if(TESSERA_ICU_FULL_DATA STREQUAL "")
    message(FATAL_ERROR
        "TESSERA_ICU_REDUCED_DATA=ON requires TESSERA_ICU_FULL_DATA=<path to "
        "icudt<version><type>.dat> (the pinned upstream ICU data package)")
endif()
if(NOT EXISTS "${TESSERA_ICU_FULL_DATA}")
    message(FATAL_ERROR "TESSERA_ICU_FULL_DATA does not exist: ${TESSERA_ICU_FULL_DATA}")
endif()

set(icu_tool_hints "")
if(NOT TESSERA_ICU_TOOLS_DIR STREQUAL "")
    set(icu_tool_hints PATHS "${TESSERA_ICU_TOOLS_DIR}" NO_DEFAULT_PATH)
endif()
find_program(TESSERA_ICU_ICUPKG
    NAMES icupkg icupkg.exe
    ${icu_tool_hints}
    DOC "Pinned ICU icupkg executable used by the reduced-data step")
if(NOT TESSERA_ICU_ICUPKG)
    message(FATAL_ERROR
        "TESSERA_ICU_REDUCED_DATA=ON requires icupkg: set TESSERA_ICU_TOOLS_DIR to the pinned "
        "ICU bin directory or put icupkg on PATH")
endif()

# The documented locale list (one ICU locale id per line, '#' comments allowed).
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/locales.txt" locale_lines)
set(locale_ids "")
foreach(line IN LISTS locale_lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    list(APPEND locale_ids "${line}")
endforeach()
if(locale_ids STREQUAL "")
    message(FATAL_ERROR "third_party/icu/locales.txt contains no locales")
endif()
list(LENGTH locale_ids TESSERA_ICU_LOCALE_COUNT)
string(REPLACE ";" ";" locale_ids "${locale_ids}")

get_filename_component(icu_dat_name "${TESSERA_ICU_FULL_DATA}" NAME_WE)
set(TESSERA_ICU_DATA_BLOB "${CMAKE_BINARY_DIR}/icu/${icu_dat_name}.dat"
    CACHE FILEPATH "Reduced ICU data package produced by the spike data step")

add_custom_command(
    OUTPUT "${TESSERA_ICU_DATA_BLOB}"
    COMMAND "${CMAKE_COMMAND}"
        "-DICU_ICUPKG=${TESSERA_ICU_ICUPKG}"
        "-DICU_FULL_DATA=${TESSERA_ICU_FULL_DATA}"
        "-DICU_OUTPUT=${TESSERA_ICU_DATA_BLOB}"
        "-DICU_LOCALES=${locale_ids}"
        -P "${CMAKE_CURRENT_LIST_DIR}/reduced_data.cmake"
    DEPENDS
        "${TESSERA_ICU_FULL_DATA}"
        "${CMAKE_CURRENT_LIST_DIR}/reduced_data.cmake"
        "${CMAKE_CURRENT_LIST_DIR}/locales.txt"
    COMMENT "Producing the reduced ICU data package (${icu_dat_name}, ${TESSERA_ICU_LOCALE_COUNT} locales)"
    VERBATIM)

add_custom_target(tessera_icu_reduced_data DEPENDS "${TESSERA_ICU_DATA_BLOB}")

message(STATUS "ICU reduced data: ${TESSERA_ICU_DATA_BLOB} (icupkg: ${TESSERA_ICU_ICUPKG})")
