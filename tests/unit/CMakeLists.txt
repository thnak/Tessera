# SPDX-License-Identifier: MIT

add_executable(merger_tests
    merger_tests.cpp)

target_link_libraries(merger_tests PRIVATE tessera_merger)

add_test(NAME merger_unit COMMAND merger_tests)

add_executable(scene_graph_tests
    scene_graph_tests.cpp)

target_link_libraries(scene_graph_tests PRIVATE tessera_scene)

add_test(NAME scene_graph COMMAND scene_graph_tests)

add_executable(scene_dirty_tests
    scene_dirty_tests.cpp)

target_link_libraries(scene_dirty_tests PRIVATE tessera_scene)

add_test(NAME scene_dirty COMMAND scene_dirty_tests)

add_executable(fixed_step_tests
    fixed_step_tests.cpp)

target_link_libraries(fixed_step_tests PRIVATE tessera_runtime)

add_test(NAME fixed_step COMMAND fixed_step_tests)

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
