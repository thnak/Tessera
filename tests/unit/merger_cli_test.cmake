# SPDX-License-Identifier: MIT
# End-to-end merger CLI test: merge with an existing file, then merge again.
# Fails if either run fails, if developer logic is lost, or if the output is not stable.

if (NOT DEFINED MERGER OR NOT DEFINED WORK OR NOT DEFINED GENERATED OR NOT DEFINED EXISTING)
    message(FATAL_ERROR "merger_cli_test.cmake requires MERGER, WORK, GENERATED, EXISTING")
endif()

file(MAKE_DIRECTORY "${WORK}")
set(first "${WORK}/merged_first.cpp")
set(second "${WORK}/merged_second.cpp")

execute_process(
    COMMAND "${MERGER}" --generated "${GENERATED}" --existing "${EXISTING}" --out "${first}"
    RESULT_VARIABLE first_result)
if (NOT first_result EQUAL 0)
    message(FATAL_ERROR "first merge failed with ${first_result}")
endif()

execute_process(
    COMMAND "${MERGER}" --generated "${GENERATED}" --existing "${first}" --out "${second}"
    RESULT_VARIABLE second_result)
if (NOT second_result EQUAL 0)
    message(FATAL_ERROR "second merge failed with ${second_result}")
endif()

file(SHA256 "${first}" first_hash)
file(SHA256 "${second}" second_hash)
if (NOT first_hash STREQUAL second_hash)
    message(FATAL_ERROR "merger is not idempotent (${first_hash} != ${second_hash})")
endif()

file(READ "${first}" content)
if (NOT content MATCHES "openPanel")
    message(FATAL_ERROR "developer logic was not preserved")
endif()
