# SPDX-License-Identifier: MIT
#
# Trim an upstream ICU data package down to the documented locale list.
#
# This runs in CMake script mode from third_party/icu/CMakeLists.txt:
#
#   cmake -DICU_ICUPKG=<path to icupkg> -DICU_FULL_DATA=<icudt76l.dat>
#         -DICU_OUTPUT=<out.dat> -DICU_LOCALES="en;de;ja"
#         -DICU_TOC_PREFIX=icudt76l -DICU_MANIFEST=<locales.json>
#         -DICU_HEADER=<icu_locale_manifest.h> -DICU_ICU_VERSION=76.1
#         -P reduced_data.cmake
#
# It removes every locale resource bundle whose locale is not in the list (plus the ancestor
# locales a listed locale falls back through), keeps all locale-independent data (Unicode
# tables, break rules and dictionaries, normalization, converters unless disabled), and then
# re-checks the package with icupkg so a locale typo or a missing fallback fails the build.
#
# It also writes two artifacts describing exactly what shipped, so a trimmed locale can never
# fail silently at runtime (docs/design/icu-data.md section 5):
#
#   locales.json            kept ids, item/byte counts, and the blob SHA-256
#   icu_locale_manifest.h   the same table compiled in for LocaleResolver
#
# Both are deterministic: sorted ids, no timestamps, no host paths.
#
# Why not `pkgdata --filter-file`: pkgdata builds a package from CLDR *source* data, and the
# pinned icu4c-76_1-src release archive ships a prebuilt `source/data/in/icudt76l.dat` with no
# per-locale `source/data/locales/*.txt` (the upstream data build regenerates them from CLDR).
# Trimming the pinned package is the only reproducible path from the pinned archive; the
# measured cost is in docs/design/icu-data.md.

cmake_minimum_required(VERSION 3.24)

foreach(required ICU_ICUPKG ICU_FULL_DATA ICU_OUTPUT ICU_LOCALES)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "reduced_data.cmake: ${required} is required")
    endif()
endforeach()

if(NOT EXISTS "${ICU_FULL_DATA}")
    message(FATAL_ERROR "reduced_data.cmake: ICU data package not found: ${ICU_FULL_DATA}")
endif()

# The ICU locale-name shape: a 2-3 letter language plus optional _Script/_REGION/_VARIANT
# subtags. ICU's non-locale bundles (pool, icustd, icuver, supplementalData, zoneinfo64, ...)
# do not match it, so they are never removed. CMake's regex engine has no {m,n} repetition,
# hence the spelled-out optional third letter and the '*' subtag repetition.
set(locale_pattern "^[a-z][a-z][a-z]?(_[A-Za-z0-9]*)*$")

execute_process(
    COMMAND "${ICU_ICUPKG}" -l "${ICU_FULL_DATA}"
    OUTPUT_VARIABLE package_items
    ERROR_VARIABLE list_error
    RESULT_VARIABLE list_result
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT list_result EQUAL 0)
    message(FATAL_ERROR "reduced_data.cmake: icupkg -l failed (${list_result}): ${list_error}")
endif()

string(REPLACE "\r" "" package_items "${package_items}")
string(REPLACE "\n" ";" package_items "${package_items}")
list(LENGTH package_items item_count)
if(item_count EQUAL 0)
    message(FATAL_ERROR "reduced_data.cmake: ${ICU_FULL_DATA} lists no items")
endif()

# Keep the listed locales and every ancestor they fall back through (de_DE -> de -> root).
set(keep_locales "${ICU_LOCALES}")
foreach(locale IN LISTS ICU_LOCALES)
    set(prefix "")
    string(REPLACE "_" ";" subtags "${locale}")
    foreach(subtag IN LISTS subtags)
        if(prefix STREQUAL "")
            set(prefix "${subtag}")
        else()
            set(prefix "${prefix}_${subtag}")
        endif()
        list(FIND package_items "${prefix}.res" locale_index)
        if(locale_index EQUAL -1)
            message(FATAL_ERROR
                "reduced_data.cmake: locale '${locale}' is not in this ICU data package "
                "('${prefix}.res' is missing); fix locales.txt")
        endif()
        list(APPEND keep_locales "${prefix}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES keep_locales)
list(SORT keep_locales)

# The ToC prefix must equal ICU's U_ICUDATA_NAME (icudt76l) so the linked blob can serve as common
# data. icupkg defaults the prefix to the *output* basename, which is deliberately icudt76 here
# (genccode derives its entry point `icudt76_dat` from that stem), hence the explicit prefix.
set(remove_list_file "${ICU_OUTPUT}.remove.txt")
set(toc_args "")
if(NOT ICU_TOC_PREFIX STREQUAL "")
    set(toc_args --toc_prefix "${ICU_TOC_PREFIX}")
endif()
foreach(pass RANGE 1 12)
    set(remove_items "")
    foreach(item IN LISTS package_items)
        get_filename_component(leaf "${item}" NAME)
        if(leaf MATCHES "\\.res$")
            string(REGEX REPLACE "\\.res$" "" stem "${leaf}")
            if(stem MATCHES "${locale_pattern}" AND NOT stem IN_LIST keep_locales)
                list(APPEND remove_items "${item}")
            endif()
        endif()
    endforeach()

    list(LENGTH remove_items remove_count)
    string(REPLACE ";" "\n" remove_text "${remove_items}")
    file(WRITE "${remove_list_file}" "${remove_text}\n")

    execute_process(
        COMMAND "${ICU_ICUPKG}" ${toc_args} -r "${remove_list_file}" "${ICU_FULL_DATA}"
                "${ICU_OUTPUT}"
        OUTPUT_VARIABLE trim_output
        ERROR_VARIABLE trim_error
        RESULT_VARIABLE trim_result)
    if(trim_result EQUAL 0)
        break()
    endif()

    set(added 0)
    string(REGEX MATCHALL "depends on missing item [^ \r\n]+" missing_deps "${trim_error}")
    foreach(dependency IN LISTS missing_deps)
        string(REGEX REPLACE ".*/|.*\\\\" "" leaf "${dependency}")
        string(REGEX REPLACE "depends on missing item " "" leaf "${leaf}")
        string(REGEX REPLACE "\\.(res|icu|nrm|cnv|brk|dict|cfu|spp)$" "" stem "${leaf}")
        if(NOT stem IN_LIST keep_locales)
            list(APPEND keep_locales "${stem}")
            math(EXPR added "${added} + 1")
        endif()
    endforeach()
    if(added EQUAL 0)
        message(FATAL_ERROR
            "reduced_data.cmake: icupkg -r failed (${trim_result}): ${trim_error}")
    endif()
endforeach()

file(REMOVE "${remove_list_file}")

execute_process(COMMAND "${ICU_ICUPKG}" -l "${ICU_OUTPUT}"
                OUTPUT_VARIABLE kept_items OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE kept_result)
if(NOT kept_result EQUAL 0)
    message(FATAL_ERROR "reduced_data.cmake: icupkg -l on the output failed (${kept_result})")
endif()
string(REPLACE "\r" "" kept_items "${kept_items}")
string(REPLACE "\n" ";" kept_items "${kept_items}")
list(LENGTH kept_items kept_count)
list(LENGTH remove_items removed_count)

file(SIZE "${ICU_FULL_DATA}" full_bytes)
file(SIZE "${ICU_OUTPUT}" reduced_bytes)
file(SHA256 "${ICU_OUTPUT}" blob_sha256)
math(EXPR percent "100 * ${reduced_bytes} / ${full_bytes}")

# Locale bundles that survived: the runtime manifest, not the requested list. The closure can
# add ids (en_GB pulls en_001), so the manifest is the only honest answer to "is this kept?".
set(kept_locale_ids "")
foreach(item IN LISTS kept_items)
    get_filename_component(leaf "${item}" NAME)
    if(leaf MATCHES "\\.res$")
        string(REGEX REPLACE "\\.res$" "" stem "${leaf}")
        if(stem MATCHES "${locale_pattern}")
            list(APPEND kept_locale_ids "${stem}")
        endif()
    endif()
endforeach()
list(REMOVE_DUPLICATES kept_locale_ids)
list(SORT kept_locale_ids)
list(LENGTH kept_locale_ids kept_locale_count)

if(DEFINED ICU_MANIFEST AND NOT ICU_MANIFEST STREQUAL "")
    foreach(locale IN LISTS kept_locale_ids)
        string(APPEND locale_json "    \"${locale}\"")
        list(FIND kept_locale_ids "${locale}" locale_pos)
        list(LENGTH kept_locale_ids locale_total)
        math(EXPR last_pos "${locale_total} - 1")
        if(NOT locale_pos EQUAL last_pos)
            string(APPEND locale_json ",\n")
        else()
            string(APPEND locale_json "\n")
        endif()
    endforeach()
    set(manifest_text
"{
  \"schema\": \"tessera.icu.locales/1\",
  \"icu_version\": \"${ICU_ICU_VERSION}\",
  \"data_name\": \"${ICU_TOC_PREFIX}\",
  \"blob\": {
    \"bytes\": ${reduced_bytes},
    \"sha256\": \"${blob_sha256}\",
    \"items\": ${kept_count}
  },
  \"locales\": [
${locale_json}  ]
}
")
    file(WRITE "${ICU_MANIFEST}" "${manifest_text}")
endif()

if(DEFINED ICU_HEADER AND NOT ICU_HEADER STREQUAL "")
    set(locale_rows "")
    foreach(locale IN LISTS kept_locale_ids)
        string(APPEND locale_rows "    \"${locale}\",\n")
    endforeach()
    file(WRITE "${ICU_HEADER}"
"// SPDX-License-Identifier: MIT
//
// Generated by third_party/icu/reduced_data.cmake from the trimmed ICU data package.
// Do not edit: it describes exactly what the linked blob contains.

#pragma once

namespace tessera::ui::text::icu {

inline constexpr char kDataName[] = \"${ICU_TOC_PREFIX}\";
inline constexpr char kIcuVersion[] = \"${ICU_ICU_VERSION}\";
inline constexpr char kBlobSha256[] = \"${blob_sha256}\";
inline constexpr long long kBlobBytes = ${reduced_bytes};
inline constexpr int kBlobItems = ${kept_count};

inline constexpr const char* kKeptLocales[] = {
${locale_rows}};

inline constexpr int kKeptLocaleCount = ${kept_locale_count};

} // namespace tessera::ui::text::icu
")
endif()

message(STATUS "tessera_icu_reduced_data: ${removed_count} items removed, ${kept_count} kept")
message(STATUS "tessera_icu_reduced_data: ${reduced_bytes} of ${full_bytes} bytes (${percent}%)")
message(STATUS "tessera_icu_reduced_data: ${kept_locale_count} locale bundles kept")
