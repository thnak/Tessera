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

add_executable(dirty_pass_tests
    dirty_pass_tests.cpp)

target_link_libraries(dirty_pass_tests PRIVATE tessera_scene)

add_test(NAME dirty_pass COMMAND dirty_pass_tests)

add_executable(fixed_step_tests
    fixed_step_tests.cpp)

target_link_libraries(fixed_step_tests PRIVATE tessera_runtime)

add_test(NAME fixed_step COMMAND fixed_step_tests)

add_executable(clock_tests
    clock_tests.cpp)

target_link_libraries(clock_tests PRIVATE tessera_runtime)

add_test(NAME clock COMMAND clock_tests)

add_executable(runtime_loop_tests
    runtime_loop_tests.cpp)

target_link_libraries(runtime_loop_tests PRIVATE tessera_runtime)

add_test(NAME runtime_loop COMMAND runtime_loop_tests)

add_executable(frame_pacer_tests
    frame_pacer_tests.cpp)

target_link_libraries(frame_pacer_tests PRIVATE tessera_runtime)

add_test(NAME frame_pacer COMMAND frame_pacer_tests)

add_executable(input_tests
    input_tests.cpp)

target_link_libraries(input_tests PRIVATE tessera_input)

add_test(NAME input COMMAND input_tests)

add_executable(damage_tests
    damage_tests.cpp)

target_link_libraries(damage_tests PRIVATE tessera_damage)

add_test(NAME damage COMMAND damage_tests)

add_executable(tokenizer_tests
    tokenizer_tests.cpp)

target_link_libraries(tokenizer_tests PRIVATE tessera_style)

add_test(NAME tokenizer COMMAND tokenizer_tests)

add_executable(parser_tests
    parser_tests.cpp)

target_link_libraries(parser_tests PRIVATE tessera_style)

add_test(NAME parser COMMAND parser_tests)

add_executable(selector_tests
    selector_tests.cpp)

target_link_libraries(selector_tests PRIVATE tessera_style)

add_test(NAME selector COMMAND selector_tests)

add_executable(selector_match_tests
    selector_match_tests.cpp)

target_link_libraries(selector_match_tests PRIVATE tessera_style)

add_test(NAME selector_match COMMAND selector_match_tests)

add_executable(cascade_tests
    cascade_tests.cpp)

target_link_libraries(cascade_tests PRIVATE tessera_style)

add_test(NAME cascade COMMAND cascade_tests)

add_executable(compiled_stylesheet_tests
    compiled_stylesheet_tests.cpp)

target_link_libraries(compiled_stylesheet_tests PRIVATE tessera_style)

add_test(NAME compiled_stylesheet COMMAND compiled_stylesheet_tests)

add_executable(property_registry_tests
    property_registry_tests.cpp)

target_link_libraries(property_registry_tests PRIVATE tessera_style)

add_test(NAME property_registry COMMAND property_registry_tests)

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
