# SPDX-License-Identifier: MIT
#
# Spike task-94: trim an upstream ICU data package down to a documented locale list.
#
# This runs in CMake script mode from third_party/icu/CMakeLists.txt:
#
#   cmake -DICU_ICUPKG=<path to icupkg> -DICU_FULL_DATA=<icudt76l.dat>
#         -DICU_OUTPUT=<out.dat> -DICU_LOCALES="en;de;ja" [-DICU_TOC_PREFIX=icudt76]
#         -P reduced_data.cmake
#
# It removes every locale resource bundle whose locale is not in the list (plus the ancestor
# locales a listed locale falls back through), keeps all locale-independent data (Unicode
# tables, break rules and dictionaries, normalization, converters unless disabled), and then
# re-checks the package with icupkg so a locale typo or a missing fallback fails the build.
#
# The locale list is intentionally a plain list of ICU locale ids; region variants are kept
# only when listed, and unlisted variants fall back to their language (documented in
# docs/design/icu-data.md).

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
                "('${prefix}.res' is missing); fix TESSERA_ICU_LOCALES")
        endif()
        list(APPEND keep_locales "${prefix}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES keep_locales)
list(SORT keep_locales)

# icupkg refuses to orphan items that reference a removed locale (en_GB needs en_001). Its
# diagnostic names the dependency, so keep that locale and retry; the keep set only grows, so
# the loop terminates.
set(remove_list_file "${ICU_OUTPUT}.remove.txt")
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
        COMMAND "${ICU_ICUPKG}" -r "${remove_list_file}" "${ICU_FULL_DATA}" "${ICU_OUTPUT}"
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
string(REPLACE "\n" ";" kept_items "${kept_items}")
list(LENGTH kept_items kept_count)
list(LENGTH remove_items removed_count)

file(SIZE "${ICU_FULL_DATA}" full_bytes)
file(SIZE "${ICU_OUTPUT}" reduced_bytes)
math(EXPR percent "100 * ${reduced_bytes} / ${full_bytes}")

message(STATUS "tessera_icu_reduced_data: ${removed_count} items removed, ${kept_count} kept")
message(STATUS "tessera_icu_reduced_data: ${reduced_bytes} of ${full_bytes} bytes (${percent}%)")
