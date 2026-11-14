# SPDX-License-Identifier: MIT
#
# CLI contract for `tessera icons add`, run by ctest as:
#   cmake -DTOOL=<exe> -DFIXTURES=<dir> -DOUT=<dir> -P run_cli_tests.cmake
#
# It writes from the pinned fixture sets only (no network), re-verifies with --check, refuses a
# stale output directory, and refuses a set whose license the gate rejects.

foreach(required TOOL FIXTURES OUT)
    if (NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "run_cli_tests: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT}")

function(run_icons result_var)
    execute_process(
        COMMAND "${TOOL}" icons add ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    set(${result_var} "${_result}" PARENT_SCOPE)
    set(ICONS_STDOUT "${_stdout}" PARENT_SCOPE)
    set(ICONS_STDERR "${_stderr}" PARENT_SCOPE)
endfunction()

run_icons(write_result tabler:home tabler:search --iconify "${FIXTURES}" --out "${OUT}")
if (NOT write_result EQUAL 0)
    message(FATAL_ERROR "icons add failed (${write_result}): ${ICONS_STDERR}")
endif()
foreach(expected icons/tabler/home.svg icons/tabler/search.svg icons.manifest icons.g.hpp
        ICON_LICENSES.txt)
    if (NOT EXISTS "${OUT}/${expected}")
        message(FATAL_ERROR "icons add did not write ${expected}")
    endif()
endforeach()

run_icons(check_result tabler:home tabler:search --iconify "${FIXTURES}" --out "${OUT}" --check)
if (NOT check_result EQUAL 0)
    message(FATAL_ERROR "icons add --check rejected its own output: ${ICONS_STDERR}")
endif()

# The reference file is an input too: same sets, tree-shaken to the listed icons.
run_icons(references_result --iconify "${FIXTURES}" --out "${OUT}" --references
    "${FIXTURES}/references.txt")
if (NOT references_result EQUAL 0)
    message(FATAL_ERROR "icons add --references failed: ${ICONS_STDERR}")
endif()
foreach(expected icons/tabler/home.svg icons/tabler/house.svg icons/tabler/search.svg
        icons/mdi/account.svg icons/ccby/chart.svg)
    if (NOT EXISTS "${OUT}/${expected}")
        message(FATAL_ERROR "the reference list did not import ${expected}")
    endif()
endforeach()

# A stale icon from an earlier import must fail --check: it would still ship in the bundle.
file(WRITE "${OUT}/icons/tabler/user.svg" "<svg/>")
run_icons(stale_file_result --iconify "${FIXTURES}" --out "${OUT}" --references
    "${FIXTURES}/references.txt" --check)
if (stale_file_result EQUAL 0)
    message(FATAL_ERROR "icons add --check accepted a stale icon file")
endif()
file(REMOVE "${OUT}/icons/tabler/user.svg")

# A different reference set means the directory is stale; --check must not pass.
run_icons(stale_result tabler:settings --iconify "${FIXTURES}" --out "${OUT}" --check)
if (stale_result EQUAL 0)
    message(FATAL_ERROR "icons add --check accepted a stale output directory")
endif()

# Rejected licenses must fail with a message that names the set, the license and the reason.
run_icons(reject_result gpl3:copyleft --iconify "${FIXTURES}" --out "${OUT}")
if (reject_result EQUAL 0)
    message(FATAL_ERROR "icons add imported a GPL set")
endif()
if (NOT ICONS_STDERR MATCHES "cannot import icon set \"gpl3\"")
    message(FATAL_ERROR "the rejection does not name the set: ${ICONS_STDERR}")
endif()
if (NOT ICONS_STDERR MATCHES "GPL-3.0")
    message(FATAL_ERROR "the rejection does not quote the license: ${ICONS_STDERR}")
endif()
if (NOT ICONS_STDERR MATCHES "GPL-family")
    message(FATAL_ERROR "the rejection does not carry the reason: ${ICONS_STDERR}")
endif()

# Share-alike needs the opt-in flag.
run_icons(share_result sharealike:share --iconify "${FIXTURES}" --out "${OUT}")
if (share_result EQUAL 0)
    message(FATAL_ERROR "icons add imported a share-alike set without the opt-in")
endif()
run_icons(share_ok_result sharealike:share --iconify "${FIXTURES}" --out "${OUT}"
    --allow-share-alike)
if (NOT share_ok_result EQUAL 0)
    message(FATAL_ERROR "icons add rejected a share-alike set with --allow-share-alike: "
        "${ICONS_STDERR}")
endif()

message(STATUS "icons_cli: ok")
