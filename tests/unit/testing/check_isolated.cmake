# SPDX-License-Identifier: MIT
#
# The `testing_isolated` check (issue #454): no shipping target may link `tessera_testing`.
#
# The issue's acceptance is "the target adds no dependency to production binaries". Keeping the kit in
# its own static library makes that visible in the build graph, but a build graph only shows what is
# linked right now; this check reads the *source* declarations, so the edit that would break the
# promise fails a test instead of waiting for a reviewer to notice.
#
# What counts as shipping: `src/`, `samples/`, `tools/` and `android/`. What may link it: `tests/`.
#
# The match is a `target_link_libraries(...)` call whose *first* argument — the target being configured
# — is not `tessera_testing` itself. That distinction is the whole check: `src/testing/CMakeLists.txt`
# legitimately writes `target_link_libraries(tessera_testing PUBLIC ...)` to declare the kit's own
# dependencies, and a naive search for the name would flag the definition instead of a consumer.
#
# Run: cmake -DSOURCE_DIR=<repo> -P check_isolated.cmake

if (NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "testing_isolated: SOURCE_DIR is not set")
endif ()

set(_shipping_roots src samples tools android)
set(_violations "")
set(_scanned 0)

foreach (_root IN LISTS _shipping_roots)
    set(_root_dir "${SOURCE_DIR}/${_root}")
    if (NOT EXISTS "${_root_dir}")
        continue()
    endif ()

    # A recursive glob on the root's own CMakeLists.txt finds every nested one (CMake treats a bare
    # filename in GLOB_RECURSE as the leaf to match at any depth).
    file(GLOB_RECURSE _lists "${_root_dir}/CMakeLists.txt")
    foreach (_list IN LISTS _lists)
        file(READ "${_list}" _content)
        # Drop comment lines, so the design note in `src/testing/CMakeLists.txt` and any doc comment
        # mentioning the target cannot trip the check.
        string(REGEX REPLACE "(^|\n)[ \t]*#[^\n]*" "\\1" _content "${_content}")
        # Every link call in the file. `[^)]` matches newlines, so a multi-line call is one match.
        string(REGEX MATCHALL "target_link_libraries[ \t\r\n]*\\([^)]*\\)" _calls "${_content}")
        foreach (_call IN LISTS _calls)
            if (NOT _call MATCHES "tessera_testing")
                continue()
            endif ()
            # The first argument is the target being configured. When it is the kit itself, this call
            # declares the kit's dependencies, not a shipping target's.
            string(REGEX MATCH "target_link_libraries[ \t\r\n]*\\([ \t\r\n]*([A-Za-z0-9_]+)" _first
                   "${_call}")
            if (CMAKE_MATCH_1 STREQUAL "tessera_testing")
                continue()
            endif ()
            list(APPEND _violations "${_list}")
        endforeach ()
        math(EXPR _scanned "${_scanned} + 1")
    endforeach ()
endforeach ()

if (_violations)
    list(REMOVE_DUPLICATES _violations)
    string(REPLACE ";" "\n  " _formatted "${_violations}")
    message(FATAL_ERROR
            "testing_isolated: a shipping target links tessera_testing:\n  ${_formatted}\n"
            "The kit is test-only (issue #454): move the link to the test target that needs it.")
endif ()

if (_scanned EQUAL 0)
    # A check that scanned nothing would pass for the wrong reason, which is the failure mode this
    # kind of guard is most likely to have.
    message(FATAL_ERROR
            "testing_isolated: scanned no CMakeLists.txt under ${SOURCE_DIR}; SOURCE_DIR is wrong")
endif ()

message(STATUS
        "testing_isolated: ${_scanned} shipping CMakeLists.txt scanned, none links tessera_testing")
