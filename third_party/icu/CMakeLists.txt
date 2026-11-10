# SPDX-License-Identifier: MIT
#
# ICU (Unicode: break iteration, normalization, case mapping, locale format data) as a real
# STATIC dependency with a reduced, linked data blob (ADR-012 rules 4-5, docs/design/icu-data.md).
#
# Why a hand-written CMake build: ICU upstream ships autoconf + MSBuild only (there is no
# icu4c/source/CMakeLists.txt in any release), and the prebuilt Windows import libraries cannot
# override their data DLL, so `udata_setCommonData` is impossible with them. Compiling the pinned
# sources here is the only way to satisfy the static-linking requirement; cmake/ThorVG.cmake uses
# the same approach for the other non-CMake upstream.
#
# Everything here is gated on TESSERA_ICU_REDUCED_DATA (declared in third_party/CMakeLists.txt,
# OFF by default), so the default build, the presets, and CI are unchanged.
#
# Produced targets:
#   tessera_icu_common  static ICU common library (Unicode core + break iteration + data reader)
#   tessera_icu_i18n    static ICU i18n library (date/number formatters)
#   tessera_icu_data    static data blob compiled from the trimmed package
#   tessera_icu         INTERFACE: headers, U_STATIC_IMPLEMENTATION, version/symbol pins
#   tessera_icu_manifest  custom target writing <build>/icu/locales.json and the locale header

if(NOT TESSERA_ICU_REDUCED_DATA)
    return()
endif()

set(TESSERA_ICU_ARCHIVE_URL
    "https://github.com/unicode-org/icu/releases/download/release-76-1/icu4c-76_1-src.tgz")
set(TESSERA_ICU_ARCHIVE_SHA256
    "dfacb46bfe4747410472ce3e1144bf28a102feeaa4e3875bac9b4c6cf30f4f3e")
set(TESSERA_ICU_EXPECTED_VERSION "76.1")

# UTF-8 little-endian ASCII is the only supported target family; ICU's data name carries the
# endianness letter and a mismatch would make the blob unreachable at runtime.
if(CMAKE_C_BYTE_ORDER STREQUAL "BIG")
    message(FATAL_ERROR "tessera_icu: big-endian targets need their own data name; unsupported")
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
foreach(tool IN ITEMS icupkg genccode)
    string(TOUPPER "${tool}" tool_upper)
    find_program(TESSERA_ICU_${tool_upper}
        NAMES ${tool} ${tool}.exe
        ${icu_tool_hints}
        DOC "Pinned ICU ${tool} executable used by the reduced-data step")
    if(NOT TESSERA_ICU_${tool_upper})
        message(FATAL_ERROR
            "TESSERA_ICU_REDUCED_DATA=ON requires ${tool}: set TESSERA_ICU_TOOLS_DIR to the "
            "pinned ICU bin directory or put it on PATH")
    endif()
endforeach()

# Pinned sources. A pre-fetched tree can be supplied for offline/CI runs; it must be the same
# release, which the version assertion below checks.
if(TESSERA_ICU_SOURCE_DIR STREQUAL "")
    include(FetchContent)
    FetchContent_Declare(tessera_icu_src
        URL "${TESSERA_ICU_ARCHIVE_URL}"
        URL_HASH SHA256=${TESSERA_ICU_ARCHIVE_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(tessera_icu_src)
    set(icu_source "${tessera_icu_src_SOURCE_DIR}")
    if(NOT EXISTS "${icu_source}/source/common")
        set(icu_source "${tessera_icu_src_SOURCE_DIR}/icu")
    endif()
else()
    set(icu_source "${TESSERA_ICU_SOURCE_DIR}")
endif()

if(NOT EXISTS "${icu_source}/source/common/unicode/uvernum.h")
    message(FATAL_ERROR "tessera_icu: no ICU sources under '${icu_source}'")
endif()

file(STRINGS "${icu_source}/source/common/unicode/uvernum.h" icu_version_lines
    REGEX "^#define U_ICU_VERSION_(MAJOR|MINOR|PATCHLEVEL)_NUM")
set(icu_version_major "")
set(icu_version_minor "")
set(icu_version_patch "")
foreach(line IN LISTS icu_version_lines)
    if(line MATCHES "MAJOR_NUM ([0-9]+)")
        set(icu_version_major "${CMAKE_MATCH_1}")
    elseif(line MATCHES "MINOR_NUM ([0-9]+)")
        set(icu_version_minor "${CMAKE_MATCH_1}")
    elseif(line MATCHES "PATCHLEVEL_NUM ([0-9]+)")
        set(icu_version_patch "${CMAKE_MATCH_1}")
    endif()
endforeach()
set(icu_version "${icu_version_major}.${icu_version_minor}")
if(NOT icu_version STREQUAL TESSERA_ICU_EXPECTED_VERSION OR NOT icu_version_patch STREQUAL "0")
    message(FATAL_ERROR
        "tessera_icu: pinned sources are ICU ${icu_version}.${icu_version_patch}, expected "
        "${TESSERA_ICU_EXPECTED_VERSION}.0")
endif()

# U_ICUDATA_NAME is 'icudt<major><type letter>' and the static entry point is
# 'icudt<major>_dat' on every ASCII platform (unicode/utypes.h); both must match the blob.
set(TESSERA_ICU_DATA_NAME "icudt${icu_version_major}l" CACHE STRING
    "ICU common-data name (ToC prefix) used for the reduced blob")
# U_ICUDATA_ENTRY_POINT (unicode/utypes.h) is 'icudt<major>_dat' and has no type letter, while the
# package ToC prefix must equal U_ICUDATA_NAME ('icudt<major>l'). genccode's -n/-e split lets one
# blob satisfy both: the file keeps the 'l' name icupkg requires, the symbol drops it.
set(TESSERA_ICU_DATA_SYMBOL "icudt${icu_version_major}_dat" CACHE STRING
    "Symbol the generated data object defines (U_ICUDATA_ENTRY_POINT)")
set(TESSERA_ICU_DATA_OBJECT "icudt${icu_version_major}" CACHE STRING
    "genccode output basename and symbol prefix")
set(TESSERA_ICU_DATA_DIR "${CMAKE_BINARY_DIR}/icu")
set(TESSERA_ICU_DATA_BLOB "${TESSERA_ICU_DATA_DIR}/${TESSERA_ICU_DATA_NAME}.dat"
    CACHE FILEPATH "Reduced ICU data package (icupkg keeps the ToC prefix equal to the basename)")
set(TESSERA_ICU_MANIFEST "${TESSERA_ICU_DATA_DIR}/locales.json" CACHE FILEPATH
    "Kept-locale manifest describing the linked blob")
set(TESSERA_ICU_LOCALE_HEADER "${TESSERA_ICU_DATA_DIR}/icu_locale_manifest.h"
    CACHE FILEPATH "Generated kept-locale table for LocaleResolver")

# The trimmer carries the ToC prefix through unchanged, so the pinned package name is what ICU will
# look up as U_ICUDATA_NAME. A mismatched package would install a blob nothing can read.
get_filename_component(icu_source_data_name "${TESSERA_ICU_FULL_DATA}" NAME_WE)
if(NOT icu_source_data_name STREQUAL TESSERA_ICU_DATA_NAME)
    message(FATAL_ERROR
        "tessera_icu: pinned package '${icu_source_data_name}' does not match the ICU data name "
        "'${TESSERA_ICU_DATA_NAME}' this build expects (U_ICUDATA_NAME)")
endif()
file(MAKE_DIRECTORY "${TESSERA_ICU_DATA_DIR}")

# --- static libraries ------------------------------------------------------------------
file(GLOB icu_common_sources CONFIGURE_DEPENDS "${icu_source}/source/common/*.cpp")
file(GLOB icu_i18n_sources CONFIGURE_DEPENDS "${icu_source}/source/i18n/*.cpp")
list(LENGTH icu_common_sources icu_common_count)
list(LENGTH icu_i18n_sources icu_i18n_count)
if(icu_common_count EQUAL 0 OR icu_i18n_count EQUAL 0)
    message(FATAL_ERROR "tessera_icu: the pinned sources contain no common/i18n translation units")
endif()

add_library(tessera_icu_common STATIC ${icu_common_sources})
target_include_directories(tessera_icu_common PUBLIC "${icu_source}/source/common")
target_compile_definitions(tessera_icu_common PRIVATE
    U_STATIC_IMPLEMENTATION U_COMMON_IMPLEMENTATION)

add_library(tessera_icu_i18n STATIC ${icu_i18n_sources})
target_include_directories(tessera_icu_i18n PUBLIC "${icu_source}/source/i18n")
target_compile_definitions(tessera_icu_i18n PRIVATE
    U_STATIC_IMPLEMENTATION U_I18N_IMPLEMENTATION)
target_link_libraries(tessera_icu_i18n PUBLIC tessera_icu_common)

foreach(icu_target IN ITEMS tessera_icu_common tessera_icu_i18n)
    # ICU 76 is a C++17 code base; building it as C++23 is not upstream-supported.
    set_target_properties(${icu_target} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${icu_target} PRIVATE /W0)
    else()
        target_compile_options(${icu_target} PRIVATE -w)
    endif()
endforeach()

# --- reduced data blob -----------------------------------------------------------------
# One script run produces the trimmed package, the manifest, and the compiled-in locale table;
# the single custom command keeps the three artifacts in sync (a stale blob can never ship
# beside a newer manifest).
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
string(REPLACE ";" "," locale_id_list "${locale_ids}")
list(LENGTH locale_ids locale_requested_count)

add_custom_command(
    OUTPUT "${TESSERA_ICU_DATA_BLOB}" "${TESSERA_ICU_MANIFEST}" "${TESSERA_ICU_LOCALE_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
        "-DICU_ICUPKG=${TESSERA_ICU_ICUPKG}"
        "-DICU_FULL_DATA=${TESSERA_ICU_FULL_DATA}"
        "-DICU_OUTPUT=${TESSERA_ICU_DATA_BLOB}"
        "-DICU_LOCALES=${locale_ids}"
        "-DICU_TOC_PREFIX=${TESSERA_ICU_DATA_NAME}"
        "-DICU_MANIFEST=${TESSERA_ICU_MANIFEST}"
        "-DICU_HEADER=${TESSERA_ICU_LOCALE_HEADER}"
        "-DICU_ICU_VERSION=${icu_version}.${icu_version_patch}"
        -P "${CMAKE_CURRENT_LIST_DIR}/reduced_data.cmake"
    DEPENDS
        "${TESSERA_ICU_FULL_DATA}"
        "${CMAKE_CURRENT_LIST_DIR}/reduced_data.cmake"
        "${CMAKE_CURRENT_LIST_DIR}/locales.txt"
    COMMENT "Producing the reduced ICU data package (${locale_requested_count} requested locales)"
    VERBATIM)

add_custom_target(tessera_icu_manifest DEPENDS
    "${TESSERA_ICU_DATA_BLOB}" "${TESSERA_ICU_MANIFEST}" "${TESSERA_ICU_LOCALE_HEADER}")

# genccode writes one C array per blob; compiling it is a single large translation unit (about
# 15 s at -O0 for the recommended tier), not a per-file pattern.
add_custom_command(
    OUTPUT "${TESSERA_ICU_DATA_DIR}/${TESSERA_ICU_DATA_OBJECT}.c"
    COMMAND "${TESSERA_ICU_GENCCODE}"
        -n "${TESSERA_ICU_DATA_OBJECT}"
        -e "${TESSERA_ICU_DATA_OBJECT}"
        -f "${TESSERA_ICU_DATA_OBJECT}"
        -d "${TESSERA_ICU_DATA_DIR}"
        "${TESSERA_ICU_DATA_BLOB}"
    DEPENDS "${TESSERA_ICU_DATA_BLOB}"
    COMMENT "Generating the ICU data object (${TESSERA_ICU_DATA_SYMBOL})"
    VERBATIM)

add_library(tessera_icu_data STATIC "${TESSERA_ICU_DATA_DIR}/${TESSERA_ICU_DATA_OBJECT}.c")
set_source_files_properties("${TESSERA_ICU_DATA_DIR}/${TESSERA_ICU_DATA_OBJECT}.c"
    PROPERTIES LANGUAGE C)
target_include_directories(tessera_icu_data PRIVATE "${icu_source}/source/common")
if(MSVC)
    target_compile_options(tessera_icu_data PRIVATE /W0)
else()
    target_compile_options(tessera_icu_data PRIVATE -w)
endif()
add_dependencies(tessera_icu_data tessera_icu_manifest)

# --- module-facing target --------------------------------------------------------------
add_library(tessera_icu INTERFACE)
target_link_libraries(tessera_icu INTERFACE
    tessera_icu_common
    tessera_icu_i18n
    tessera_icu_data)
target_include_directories(tessera_icu INTERFACE
    "${icu_source}/source/common"
    "${TESSERA_ICU_DATA_DIR}")
# Consumers must see U_STATIC_IMPLEMENTATION: without it ICU headers declare dllimport and the
# link fails against the static archives.
target_compile_definitions(tessera_icu INTERFACE
    U_STATIC_IMPLEMENTATION
    TESSERA_ICU_DATA_SYMBOL=${TESSERA_ICU_DATA_SYMBOL}
    TESSERA_ICU_DATA_NAME="${TESSERA_ICU_DATA_NAME}"
    TESSERA_ICU_VERSION_MAJOR=${icu_version_major})

message(STATUS "tessera_icu: static ICU ${icu_version}.${icu_version_patch} "
    "(${icu_common_count} common + ${icu_i18n_count} i18n sources)")
