# SPDX-License-Identifier: MIT
#
# Drives one negative compile and asserts WHERE its diagnostics point (issue #78). The mapped base of
# tests/fixtures/codegen-line-map/Two.tsx carries `#line` directives, so the two missing-binding
# failures must report TSX coordinates instead of lines of `Two.g.hpp`.
#
# The expected lines are read from the fixture, not hardcoded: every line of the TSX carrying a
# `// Note: bind::` marker is one the compiler must name. A fixture edit therefore cannot silently
# weaken the test, and no magic number can drift out of date.
#
# Two mapped errors, not one: a final pass that restores the generated numbering wrongly (off by the
# number of directives it emitted) still gets the FIRST diagnostic right and lies about every later
# one. The second error is what catches that, so the fixture has two markers and the check requires
# both locations.
#
# The match is on the diagnostic's location prefix, never on the message text: the
# TESSERA_CG_MISSING_BINDING message already embeds the TSX location in BOTH variants, so a substring
# search would pass without any mapping at all. Requiring the line to start with the file location
# and to name the error class is what proves the compiler resolved the position through `#line`.
#
# Inputs: BUILD_DIR, CONFIG, TARGET, TSX (absolute fixture path), SOURCE (its file name).

if (NOT DEFINED BUILD_DIR OR NOT DEFINED TARGET OR NOT DEFINED TSX OR NOT DEFINED SOURCE)
    message(FATAL_ERROR "line_map_compile_check.cmake needs BUILD_DIR, TARGET, TSX and SOURCE")
endif ()

file(STRINGS "${TSX}" _tsx_lines)
set(_expected "")
set(_line_number 0)
foreach (_line IN LISTS _tsx_lines)
    math(EXPR _line_number "${_line_number} + 1")
    if (_line MATCHES "Note: bind::")
        list(APPEND _expected "${_line_number}")
    endif ()
endforeach ()
list(LENGTH _expected _expected_count)
if (_expected_count LESS 2)
    message(FATAL_ERROR "the fixture must carry at least two `Note: bind::` markers, found "
                        "${_expected_count}: a single mapped error cannot prove the restored "
                        "numbering is correct")
endif ()

execute_process(
    COMMAND ${CMAKE_COMMAND} --build ${BUILD_DIR} --config ${CONFIG} --target ${TARGET}
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)

set(_combined "${_out}\n${_err}")
if (_result EQUAL 0)
    message(FATAL_ERROR "the negative compile unexpectedly succeeded: the binding contract no "
                        "longer fails, so this test proves nothing")
endif ()

string(REPLACE "\n" ";" _lines "${_combined}")
set(_mapped 0)
foreach (_expected_line IN LISTS _expected)
    set(_found FALSE)
    foreach (_line IN LISTS _lines)
        # MSVC: `.../Two.tsx(10): error C2338: ...`; GCC/Clang: `Two.tsx:10:9: error: ...`. The
        # location must be the line's first token and an error must follow it, which is what keeps
        # the location quoted inside the assertion message from counting.
        if (_line MATCHES "^[ \t]*[^ \t]*${SOURCE}[:(]${_expected_line}[),:].*[Ee]rror")
            set(_found TRUE)
            break()
        endif ()
    endforeach ()
    if (_found)
        math(EXPR _mapped "${_mapped} + 1")
        message(STATUS "mapped diagnostic: ${SOURCE} line ${_expected_line}")
    else ()
        message(STATUS "MISSING: no diagnostic named ${SOURCE} line ${_expected_line}")
    endif ()
endforeach ()

if (_mapped LESS _expected_count)
    message(FATAL_ERROR
            "expected every one of ${_expected_count} diagnostics to name a TSX line "
            "(${_expected}), got ${_mapped}. The `#line` map did not reach the generated base, or the "
            "generated-file numbering was not restored correctly after the first mapped region.\n"
            "--- compiler output ---\n${_combined}")
endif ()
