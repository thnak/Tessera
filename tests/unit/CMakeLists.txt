# SPDX-License-Identifier: MIT

add_executable(merger_tests
    merger_tests.cpp)

target_link_libraries(merger_tests PRIVATE tessera_merger)

add_test(NAME merger_unit COMMAND merger_tests)

if (TARGET merger_poc)
    add_test(NAME merger_cli_help COMMAND merger_poc --help)
    add_test(
        NAME merger_cli_idempotent
        COMMAND ${CMAKE_COMMAND}
            -DMERGER=$<TARGET_FILE:merger_poc>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/merger_cli
            -DGENERATED=${CMAKE_SOURCE_DIR}/tests/fixtures/merger/generated.cpp
            -DEXISTING=${CMAKE_SOURCE_DIR}/tests/fixtures/merger/existing.cpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/merger_cli_test.cmake)
endif()
