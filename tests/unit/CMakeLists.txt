# SPDX-License-Identifier: MIT

add_executable(merger_tests
    merger_tests.cpp)

target_link_libraries(merger_tests PRIVATE tessera_merger)

add_test(NAME merger_unit COMMAND merger_tests)

add_executable(scene_graph_tests
    scene_graph_tests.cpp)

target_link_libraries(scene_graph_tests PRIVATE tessera_scene)

add_test(NAME scene_graph COMMAND scene_graph_tests)

add_executable(input_dispatch_tests
    input_dispatch_tests.cpp)

target_link_libraries(input_dispatch_tests PRIVATE tessera_scene)

add_test(NAME input_dispatch COMMAND input_dispatch_tests)

add_executable(input_router_tests
    input_router_tests.cpp)

target_link_libraries(input_router_tests PRIVATE tessera_scene)

add_test(NAME input_router COMMAND input_router_tests)

add_executable(focus_tests
    focus_tests.cpp)

target_link_libraries(focus_tests PRIVATE tessera_scene)

add_test(NAME focus COMMAND focus_tests)

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

add_executable(hal_timer_tests
    hal_timer_tests.cpp)

target_link_libraries(hal_timer_tests PRIVATE tessera_runtime tessera_hal_timer)

add_test(NAME hal_timer COMMAND hal_timer_tests)

add_executable(window_metrics_tests
    window_metrics_tests.cpp)

target_include_directories(window_metrics_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include)

add_test(NAME window_metrics COMMAND window_metrics_tests)

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

add_executable(diagnostics_log_tests
    diagnostics_log_tests.cpp)

target_link_libraries(diagnostics_log_tests PRIVATE tessera_diagnostics)

add_test(NAME diagnostics_log COMMAND diagnostics_log_tests)

add_executable(diagnostics_metrics_tests
    diagnostics_metrics_tests.cpp)

target_link_libraries(diagnostics_metrics_tests PRIVATE tessera_diagnostics)

add_test(NAME diagnostics_metrics COMMAND diagnostics_metrics_tests)

add_executable(diagnostics_trace_tests
    diagnostics_trace_tests.cpp)

target_link_libraries(diagnostics_trace_tests PRIVATE tessera_diagnostics)

add_test(NAME diagnostics_trace COMMAND diagnostics_trace_tests)

add_executable(diagnostics_overlay_tests
    diagnostics_overlay_tests.cpp)

target_link_libraries(diagnostics_overlay_tests PRIVATE tessera_diagnostics)

add_test(NAME diagnostics_overlay COMMAND diagnostics_overlay_tests)

add_executable(async_tests
    async_tests.cpp)

target_link_libraries(async_tests PRIVATE tessera_runtime)

add_test(NAME async COMMAND async_tests)

add_executable(completion_queue_tests
    completion_queue_tests.cpp)

target_link_libraries(completion_queue_tests PRIVATE tessera_runtime)

add_test(NAME completion_queue COMMAND completion_queue_tests)

add_executable(worker_executor_tests
    worker_executor_tests.cpp)

target_link_libraries(worker_executor_tests PRIVATE tessera_runtime)

add_test(NAME worker_executor COMMAND worker_executor_tests)

add_executable(task_tests
    task_tests.cpp)

target_link_libraries(task_tests PRIVATE tessera_runtime)

add_test(NAME task COMMAND task_tests)

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

add_executable(animation_pause_tests
    animation_pause_tests.cpp)

target_link_libraries(animation_pause_tests PRIVATE tessera_animation)

add_test(NAME animation_pause COMMAND animation_pause_tests)

add_executable(animation_stop_tests
    animation_stop_tests.cpp)

target_link_libraries(animation_stop_tests PRIVATE tessera_animation)

add_test(NAME animation_stop COMMAND animation_stop_tests)

add_executable(animation_fill_tests
    animation_fill_tests.cpp)

target_link_libraries(animation_fill_tests PRIVATE tessera_animation)

add_test(NAME animation_fill COMMAND animation_fill_tests)

add_executable(animation_stop_name_tests
    animation_stop_name_tests.cpp)

target_link_libraries(animation_stop_name_tests PRIVATE tessera_animation)

add_test(NAME animation_stop_name COMMAND animation_stop_name_tests)

add_executable(animation_restart_tests
    animation_restart_tests.cpp)

target_link_libraries(animation_restart_tests PRIVATE tessera_animation)

add_test(NAME animation_restart COMMAND animation_restart_tests)

add_executable(animation_pause_name_tests
    animation_pause_name_tests.cpp)

target_link_libraries(animation_pause_name_tests PRIVATE tessera_animation)

add_test(NAME animation_pause_name COMMAND animation_pause_name_tests)

add_executable(animation_state_tests
    animation_state_tests.cpp)

target_link_libraries(animation_state_tests PRIVATE tessera_animation)

add_test(NAME animation_state COMMAND animation_state_tests)

add_executable(animation_play_state_tests
    animation_play_state_tests.cpp)

target_link_libraries(animation_play_state_tests PRIVATE tessera_animation)

add_test(NAME animation_play_state COMMAND animation_play_state_tests)

add_executable(animation_ensure_tests
    animation_ensure_tests.cpp)

target_link_libraries(animation_ensure_tests PRIVATE tessera_animation)

add_test(NAME animation_ensure COMMAND animation_ensure_tests)

add_executable(animation_sync_tests
    animation_sync_tests.cpp)

target_link_libraries(animation_sync_tests PRIVATE tessera_animation)

add_test(NAME animation_sync COMMAND animation_sync_tests)

add_executable(animation_loop_tests
    animation_loop_tests.cpp)

target_link_libraries(animation_loop_tests PRIVATE tessera_animation_loop)

add_test(NAME animation_loop COMMAND animation_loop_tests)

add_executable(animation_callback_tests
    animation_callback_tests.cpp)

target_link_libraries(animation_callback_tests PRIVATE tessera_animation)

add_test(NAME animation_callback COMMAND animation_callback_tests)

add_executable(animation_restart_id_tests
    animation_restart_id_tests.cpp)

target_link_libraries(animation_restart_id_tests PRIVATE tessera_animation)

add_test(NAME animation_restart_id COMMAND animation_restart_id_tests)

add_executable(animation_tick_reentrancy_tests
    animation_tick_reentrancy_tests.cpp)

target_link_libraries(animation_tick_reentrancy_tests PRIVATE tessera_animation)

add_test(NAME animation_tick_reentrancy COMMAND animation_tick_reentrancy_tests)

add_executable(animation_reduced_motion_tests
    animation_reduced_motion_tests.cpp)

target_link_libraries(animation_reduced_motion_tests PRIVATE tessera_animation)

add_test(NAME animation_reduced_motion COMMAND animation_reduced_motion_tests)

add_executable(easing_tests
    easing_tests.cpp)

target_link_libraries(easing_tests PRIVATE tessera_animation)

add_test(NAME easing COMMAND easing_tests)

add_executable(spring_tests
    spring_tests.cpp)

target_link_libraries(spring_tests PRIVATE tessera_animation)

add_test(NAME spring COMMAND spring_tests)

add_executable(spring_runtime_tests
    spring_runtime_tests.cpp)

target_link_libraries(spring_runtime_tests PRIVATE tessera_animation)

add_test(NAME spring_runtime COMMAND spring_runtime_tests)

add_executable(animation_overlay_tests
    animation_overlay_tests.cpp)

target_link_libraries(animation_overlay_tests PRIVATE tessera_animation)

add_test(NAME animation_overlay COMMAND animation_overlay_tests)

add_executable(transition_tests
    transition_tests.cpp)

target_link_libraries(transition_tests PRIVATE tessera_animation)

add_test(NAME transition COMMAND transition_tests)

add_executable(transition_runtime_tests
    transition_runtime_tests.cpp)

target_link_libraries(transition_runtime_tests PRIVATE tessera_animation)

add_test(NAME transition_runtime COMMAND transition_runtime_tests)

add_executable(transition_lifecycle_tests
    transition_lifecycle_tests.cpp)

target_link_libraries(transition_lifecycle_tests PRIVATE tessera_animation)

add_test(NAME transition_lifecycle COMMAND transition_lifecycle_tests)

add_executable(transition_longhand_tests
    transition_longhand_tests.cpp)

target_link_libraries(transition_longhand_tests PRIVATE tessera_animation)

add_test(NAME transition_longhand COMMAND transition_longhand_tests)

add_executable(transition_pipeline_tests
    transition_pipeline_tests.cpp)

target_link_libraries(transition_pipeline_tests PRIVATE tessera_animation)

add_test(NAME transition_pipeline COMMAND transition_pipeline_tests)

add_executable(animation_shorthand_tests
    animation_shorthand_tests.cpp)

target_link_libraries(animation_shorthand_tests PRIVATE tessera_animation)

add_test(NAME animation_shorthand COMMAND animation_shorthand_tests)

add_executable(animation_start_tests
    animation_start_tests.cpp)

target_link_libraries(animation_start_tests PRIVATE tessera_animation)

add_test(NAME animation_start COMMAND animation_start_tests)

add_executable(animation_longhand_tests
    animation_longhand_tests.cpp)

target_link_libraries(animation_longhand_tests PRIVATE tessera_animation)

add_test(NAME animation_longhand COMMAND animation_longhand_tests)

add_executable(animation_lookup_tests
    animation_lookup_tests.cpp)

target_link_libraries(animation_lookup_tests PRIVATE tessera_animation)

add_test(NAME animation_lookup COMMAND animation_lookup_tests)

add_executable(animation_pipeline_tests
    animation_pipeline_tests.cpp)

target_link_libraries(animation_pipeline_tests PRIVATE tessera_animation)

add_test(NAME animation_pipeline COMMAND animation_pipeline_tests)

add_executable(animation_style_tests
    animation_style_tests.cpp)

target_link_libraries(animation_style_tests PRIVATE tessera_animation)

add_test(NAME animation_style COMMAND animation_style_tests)

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

add_executable(font_metrics_tests
    font_metrics_tests.cpp)

target_link_libraries(font_metrics_tests PRIVATE tessera_text)

add_test(NAME font_metrics COMMAND font_metrics_tests)

set(TESSERA_FONT_FIXTURE
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/NotoSans-Regular.ttf")
get_filename_component(TESSERA_FONT_FIXTURE "${TESSERA_FONT_FIXTURE}" REALPATH)

add_executable(font_face_tests
    font_face_tests.cpp)

target_link_libraries(font_face_tests PRIVATE tessera_text)

add_test(NAME font_face COMMAND font_face_tests "${TESSERA_FONT_FIXTURE}")

add_executable(freetype_metrics_tests
    freetype_metrics_tests.cpp)

target_link_libraries(freetype_metrics_tests PRIVATE tessera_text)

add_test(NAME freetype_metrics COMMAND freetype_metrics_tests "${TESSERA_FONT_FIXTURE}")

add_executable(glyph_raster_tests
    glyph_raster_tests.cpp)

target_link_libraries(glyph_raster_tests PRIVATE tessera_text)

add_test(NAME glyph_raster COMMAND glyph_raster_tests "${TESSERA_FONT_FIXTURE}")

add_executable(glyph_atlas_tests
    glyph_atlas_tests.cpp)

target_link_libraries(glyph_atlas_tests PRIVATE tessera_text)

add_test(NAME glyph_atlas COMMAND glyph_atlas_tests "${TESSERA_FONT_FIXTURE}")

add_executable(harfbuzz_shaper_tests
    harfbuzz_shaper_tests.cpp)

target_link_libraries(harfbuzz_shaper_tests PRIVATE tessera_text)

add_test(NAME harfbuzz_shaper COMMAND harfbuzz_shaper_tests "${TESSERA_FONT_FIXTURE}")

add_executable(text_layout_tests
    text_layout_tests.cpp)

target_link_libraries(text_layout_tests PRIVATE tessera_text)

add_test(NAME text_layout COMMAND text_layout_tests)

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

add_executable(frame_pipeline_tests
    frame_pipeline_tests.cpp)

target_link_libraries(frame_pipeline_tests PRIVATE tessera_frame tessera_runtime)

add_test(NAME frame_pipeline COMMAND frame_pipeline_tests)

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

add_executable(image_diff_tests
    image_diff_tests.cpp)

target_link_libraries(image_diff_tests PRIVATE tessera_golden)

add_test(NAME image_diff COMMAND image_diff_tests)

add_executable(swapchain_tests
    swapchain_tests.cpp)

target_link_libraries(swapchain_tests PRIVATE tessera_rhi_null)

add_test(NAME swapchain COMMAND swapchain_tests)
