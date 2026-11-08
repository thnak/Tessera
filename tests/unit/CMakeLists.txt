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

add_executable(runtime_loop_async_tests
    runtime_loop_async_tests.cpp)

target_link_libraries(runtime_loop_async_tests PRIVATE tessera_runtime)

add_test(NAME runtime_loop_async COMMAND runtime_loop_async_tests)

add_executable(diagnostics_tests
    diagnostics_tests.cpp)

target_link_libraries(diagnostics_tests PRIVATE tessera_runtime)

add_test(NAME diagnostics COMMAND diagnostics_tests)

add_executable(async_tests
    async_tests.cpp)

target_link_libraries(async_tests PRIVATE tessera_runtime)

add_test(NAME async COMMAND async_tests)

add_executable(completion_queue_tests
    completion_queue_tests.cpp)

target_link_libraries(completion_queue_tests PRIVATE tessera_runtime)

add_test(NAME completion_queue COMMAND completion_queue_tests)

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

add_executable(redraw_plan_tests
    redraw_plan_tests.cpp)

target_link_libraries(redraw_plan_tests PRIVATE tessera_damage)

add_test(NAME redraw_plan COMMAND redraw_plan_tests)

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

add_executable(media_query_tests
    media_query_tests.cpp)

target_link_libraries(media_query_tests PRIVATE tessera_style)

add_test(NAME media_query COMMAND media_query_tests)

add_executable(keyframes_tests
    keyframes_tests.cpp)

target_link_libraries(keyframes_tests PRIVATE tessera_style)

add_test(NAME keyframes COMMAND keyframes_tests)

add_executable(supports_tests
    supports_tests.cpp)

target_link_libraries(supports_tests PRIVATE tessera_style)

add_test(NAME supports COMMAND supports_tests)

add_executable(color_tests
    color_tests.cpp)

target_link_libraries(color_tests PRIVATE tessera_style)

add_test(NAME color COMMAND color_tests)

add_executable(animation_tests
    animation_tests.cpp)

target_link_libraries(animation_tests PRIVATE tessera_animation)

add_test(NAME animation COMMAND animation_tests)

add_executable(animation_runtime_tests
    animation_runtime_tests.cpp)

target_link_libraries(animation_runtime_tests PRIVATE tessera_animation)

add_test(NAME animation_runtime COMMAND animation_runtime_tests)

add_executable(easing_tests
    easing_tests.cpp)

target_link_libraries(easing_tests PRIVATE tessera_animation)

add_test(NAME easing COMMAND easing_tests)

add_executable(spring_tests
    spring_tests.cpp)

target_link_libraries(spring_tests PRIVATE tessera_animation)

add_test(NAME spring COMMAND spring_tests)

add_executable(layout_tests
    layout_tests.cpp)

target_link_libraries(layout_tests PRIVATE tessera_layout)

add_test(NAME layout COMMAND layout_tests)

add_executable(layout_cache_tests
    layout_cache_tests.cpp)

target_link_libraries(layout_cache_tests PRIVATE tessera_layout)

add_test(NAME layout_cache COMMAND layout_cache_tests)

add_executable(text_measure_tests
    text_measure_tests.cpp)

target_link_libraries(text_measure_tests PRIVATE tessera_text)

add_test(NAME text_measure COMMAND text_measure_tests)

add_executable(text_measure_cache_tests
    text_measure_cache_tests.cpp)

target_link_libraries(text_measure_cache_tests PRIVATE tessera_text)

add_test(NAME text_measure_cache COMMAND text_measure_cache_tests)

add_executable(layout_apply_tests
    layout_apply_tests.cpp)

target_link_libraries(layout_apply_tests PRIVATE tessera_layout)

add_test(NAME layout_apply COMMAND layout_apply_tests)

add_executable(pixel_snap_tests
    pixel_snap_tests.cpp)

target_link_libraries(pixel_snap_tests PRIVATE tessera_layout)

add_test(NAME pixel_snap COMMAND pixel_snap_tests)

add_executable(damage_pass_tests
    damage_pass_tests.cpp)

target_link_libraries(damage_pass_tests PRIVATE tessera_layout)

add_test(NAME damage_pass COMMAND damage_pass_tests)

add_executable(damage_tracker_tests
    damage_tracker_tests.cpp)

target_link_libraries(damage_tracker_tests PRIVATE tessera_layout)

add_test(NAME damage_tracker COMMAND damage_tracker_tests)

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
