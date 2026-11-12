# SPDX-License-Identifier: MIT

add_executable(merger_tests
    merger_tests.cpp)

target_link_libraries(merger_tests PRIVATE tessera_merger)

add_test(NAME merger_unit COMMAND merger_tests)

add_executable(jsx_lexer_tests
    jsx_lexer_tests.cpp)

target_link_libraries(jsx_lexer_tests PRIVATE tessera_jsx_frontend)

add_test(NAME jsx_lexer COMMAND jsx_lexer_tests)

add_executable(jsx_expression_tests
    jsx_expression_tests.cpp)

target_link_libraries(jsx_expression_tests PRIVATE tessera_jsx_frontend)

add_test(NAME jsx_expression COMMAND jsx_expression_tests)

add_executable(jsx_parser_tests
    jsx_parser_tests.cpp)

target_link_libraries(jsx_parser_tests PRIVATE tessera_jsx_frontend)

add_test(NAME jsx_parser COMMAND jsx_parser_tests)

add_executable(jsx_type_tests
    jsx_type_tests.cpp)

target_link_libraries(jsx_type_tests PRIVATE tessera_jsx_frontend)

add_test(NAME jsx_type COMMAND jsx_type_tests)

add_executable(jsx_module_tests
    jsx_module_tests.cpp)

target_link_libraries(jsx_module_tests PRIVATE tessera_jsx_frontend)

add_test(NAME jsx_module COMMAND jsx_module_tests)

find_package(Threads REQUIRED)

add_executable(jsx_concurrent_tests
    jsx_concurrent_tests.cpp)

target_link_libraries(jsx_concurrent_tests PRIVATE tessera_jsx_frontend Threads::Threads)

add_test(NAME jsx_concurrent COMMAND jsx_concurrent_tests)

add_executable(jsx_bind_tests
    jsx_bind_tests.cpp)

target_link_libraries(jsx_bind_tests PRIVATE tessera_jsx_frontend)

add_test(NAME jsx_bind COMMAND jsx_bind_tests)

add_executable(jsx_rules_tests
    jsx_rules_tests.cpp)

target_link_libraries(jsx_rules_tests PRIVATE tessera_jsx_frontend)

add_test(NAME jsx_rules COMMAND jsx_rules_tests)

add_executable(jsx_adversarial_tests
    jsx_adversarial_tests.cpp)

target_link_libraries(jsx_adversarial_tests PRIVATE tessera_jsx_frontend)

add_test(NAME jsx_adversarial COMMAND jsx_adversarial_tests)

add_executable(ir_json_value_tests
    ir_json_value_tests.cpp)

target_link_libraries(ir_json_value_tests PRIVATE tessera_ir)

add_test(NAME ir_json_value COMMAND ir_json_value_tests)

add_executable(ir_json_tests
    ir_json_tests.cpp
    ir_samples.cpp
    ir_samples_rich.cpp)

target_link_libraries(ir_json_tests PRIVATE tessera_ir)

target_compile_definitions(ir_json_tests PRIVATE
    TESSERA_IR_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/ir")

add_test(NAME ir_json COMMAND ir_json_tests)

add_executable(ir_validate_tests
    ir_validate_tests.cpp
    ir_samples.cpp
    ir_samples_rich.cpp
    ir_samples_valid.cpp)

target_link_libraries(ir_validate_tests PRIVATE tessera_ir)

add_test(NAME ir_validate COMMAND ir_validate_tests)

add_executable(ir_validate_shape_tests
    ir_validate_shape_tests.cpp
    ir_samples.cpp
    ir_samples_rich.cpp
    ir_samples_valid.cpp)

target_link_libraries(ir_validate_shape_tests PRIVATE tessera_ir)

add_test(NAME ir_validate_shape COMMAND ir_validate_shape_tests)

add_executable(ir_validate_expr_tests
    ir_validate_expr_tests.cpp
    ir_samples.cpp
    ir_samples_rich.cpp
    ir_samples_valid.cpp)

target_link_libraries(ir_validate_expr_tests PRIVATE tessera_ir)

add_test(NAME ir_validate_expr COMMAND ir_validate_expr_tests)

add_executable(ir_construct_tests
    ir_construct_tests.cpp
    ir_samples.cpp
    ir_samples_rich.cpp
    ir_samples_valid.cpp)

target_link_libraries(ir_construct_tests PRIVATE
    tessera_ir
    Threads::Threads)

add_test(NAME ir_construct COMMAND ir_construct_tests)

add_executable(ir_layering_tests
    ir_layering_tests.cpp)

target_compile_definitions(ir_layering_tests PRIVATE
    TESSERA_COMPILER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/compiler")

add_test(NAME ir_layering COMMAND ir_layering_tests)

add_executable(rules_hooks_tests
    rules_hooks_tests.cpp)

target_link_libraries(rules_hooks_tests PRIVATE tessera_react_rules)

add_test(NAME rules_hooks COMMAND rules_hooks_tests)

add_executable(rules_arrows_tests
    rules_arrows_tests.cpp)

target_link_libraries(rules_arrows_tests PRIVATE tessera_react_rules)

add_test(NAME rules_arrows COMMAND rules_arrows_tests)

add_executable(rules_effect_deps_tests
    rules_effect_deps_tests.cpp)

target_link_libraries(rules_effect_deps_tests PRIVATE tessera_react_rules)

add_test(NAME rules_effect_deps COMMAND rules_effect_deps_tests)

add_executable(rules_concurrent_tests
    rules_concurrent_tests.cpp)

target_link_libraries(rules_concurrent_tests PRIVATE tessera_react_rules Threads::Threads)

add_test(NAME rules_concurrent COMMAND rules_concurrent_tests)

add_executable(rules_layering_tests
    rules_layering_tests.cpp)

target_compile_definitions(rules_layering_tests PRIVATE
    TESSERA_COMPILER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/compiler")

add_test(NAME rules_layering COMMAND rules_layering_tests)

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

add_executable(click_synthesis_tests
    click_synthesis_tests.cpp)

target_link_libraries(click_synthesis_tests PRIVATE tessera_scene)

add_test(NAME click_synthesis COMMAND click_synthesis_tests)

add_executable(focus_tests
    focus_tests.cpp)

target_link_libraries(focus_tests PRIVATE tessera_scene)

add_test(NAME focus COMMAND focus_tests)

add_executable(focus_policy_tests
    focus_policy_tests.cpp)

target_link_libraries(focus_policy_tests PRIVATE tessera_scene)

add_test(NAME focus_policy COMMAND focus_policy_tests)

add_executable(focus_style_tests
    focus_style_tests.cpp)

target_link_libraries(focus_style_tests PRIVATE tessera_style)

add_test(NAME focus_style COMMAND focus_style_tests)

add_executable(scene_style_metadata_tests
    scene_style_metadata_tests.cpp)

target_link_libraries(scene_style_metadata_tests PRIVATE tessera_style)

add_test(NAME scene_style_metadata COMMAND scene_style_metadata_tests)

add_executable(app_host_tests
    app_host_tests.cpp)

target_link_libraries(app_host_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME app_host COMMAND app_host_tests)

# Embedded stylesheet registration (issue #81, ADR-048): blobs load in table (import-graph) order,
# a later sheet wins an equal cascade key, and a corrupt blob is reported and skipped. Painting goes
# through a deterministic CPU sink; no GPU, font or wall clock.
add_executable(app_host_stylesheet_tests
    app_host_stylesheet_tests.cpp)

target_link_libraries(app_host_stylesheet_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME app_host_stylesheet COMMAND app_host_stylesheet_tests)

# The window driver stops after a bounded run of frame failures instead of spinning or hanging.
add_executable(app_frame_guard_tests
    app_frame_guard_tests.cpp)

target_link_libraries(app_frame_guard_tests PRIVATE tessera_app)

add_test(NAME app_frame_guard COMMAND app_frame_guard_tests)

# The window driver idles on the pacer deadline (never a fixed sleep), never sleeps past it, and
# presents only frames whose damage plan has regions.
add_executable(app_idle_tests
    app_idle_tests.cpp)

target_link_libraries(app_idle_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME app_idle COMMAND app_idle_tests)

# Bounded device-loss recovery policy: a transient loss recovers once, a persistent loss stops after
# the explicit attempt budget instead of retrying forever.
add_executable(app_recovery_policy_tests
    app_recovery_policy_tests.cpp)

target_link_libraries(app_recovery_policy_tests PRIVATE tessera_app)

add_test(NAME app_recovery_policy COMMAND app_recovery_policy_tests)

# Input through the window pump: hover/active, click synthesis and typed text on the generated Demo.
# The generated demo target only exists on host builds.
if (TARGET tessera_compiler_demo)
    add_executable(app_input_tests
        app_input_tests.cpp)
    target_include_directories(app_input_tests PRIVATE
        ${CMAKE_BINARY_DIR}/src/compiler/codegen/samples/compiler-demo)
    target_link_libraries(app_input_tests PRIVATE
        tessera_app
        tessera_compiler_demo
        tessera_input_pump
        tessera_paint)
    add_test(NAME app_input COMMAND app_input_tests)
endif()

add_executable(scene_dirty_tests
    scene_dirty_tests.cpp)

target_link_libraries(scene_dirty_tests PRIVATE tessera_scene)

add_test(NAME scene_dirty COMMAND scene_dirty_tests)

add_executable(update_scheduler_tests
    update_scheduler_tests.cpp)

target_link_libraries(update_scheduler_tests PRIVATE tessera_scene)

add_test(NAME update_scheduler COMMAND update_scheduler_tests)

add_executable(node_size_tests
    node_size_tests.cpp)

target_link_libraries(node_size_tests PRIVATE tessera_scene)

add_test(NAME node_size COMMAND node_size_tests)

add_executable(node_metadata_tests
    node_metadata_tests.cpp)

target_link_libraries(node_metadata_tests PRIVATE tessera_scene)

add_test(NAME node_metadata COMMAND node_metadata_tests)

add_executable(scene_event_binding_tests
    scene_event_binding_tests.cpp)

target_link_libraries(scene_event_binding_tests PRIVATE tessera_scene)

add_test(NAME scene_event_binding COMMAND scene_event_binding_tests)

add_executable(scene_scroll_event_tests
    scene_scroll_event_tests.cpp)

target_link_libraries(scene_scroll_event_tests PRIVATE tessera_scene)

add_test(NAME scene_scroll_event COMMAND scene_scroll_event_tests)

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

if (WIN32)
    add_executable(window_win32_tests
        window_win32_tests.cpp)

    target_link_libraries(window_win32_tests PRIVATE tessera_hal_window)

    add_test(NAME window_win32 COMMAND window_win32_tests)

    add_executable(window_win32_input_tests
        window_win32_input_tests.cpp)

    target_link_libraries(window_win32_input_tests PRIVATE tessera_hal_window tessera_input)

    # The pure pointer/touch translation decisions are tested without an HWND or a device.
    target_include_directories(window_win32_input_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/windows)

    add_test(NAME window_win32_input COMMAND window_win32_input_tests)

    add_executable(win32_ime_tests
        win32_ime_tests.cpp)

    target_link_libraries(win32_ime_tests PRIVATE tessera_hal_window)

    # Pure IME translation decisions are tested without an HWND or an IME.
    target_include_directories(win32_ime_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/platform/windows)

    add_test(NAME win32_ime COMMAND win32_ime_tests)

    add_executable(window_wait_tests
        window_wait_tests.cpp)

    target_link_libraries(window_wait_tests PRIVATE tessera_hal_window)

    add_test(NAME window_wait COMMAND window_wait_tests)
endif()

if (TESSERA_X11_FOUND)
    add_executable(window_x11_tests
        window_x11_tests.cpp)

    target_link_libraries(window_x11_tests PRIVATE
        tessera_hal_window
        X11::X11)

    add_test(NAME window_x11 COMMAND window_x11_tests)

    add_executable(window_x11_input_tests
        window_x11_input_tests.cpp)

    target_link_libraries(window_x11_input_tests PRIVATE
        tessera_hal_window
        tessera_input
        X11::X11)

    add_test(NAME window_x11_input COMMAND window_x11_input_tests)

    # Real server-generated input through the XTEST extension (issue #10); needs libXtst.
    if (TARGET X11::Xtst)
        add_executable(window_x11_real_input_tests
            window_x11_real_input_tests.cpp)

        target_link_libraries(window_x11_real_input_tests PRIVATE
            tessera_hal_window
            tessera_input
            X11::X11
            X11::Xtst)

        add_test(NAME window_x11_real_input COMMAND window_x11_real_input_tests)
    endif()
endif()

# The pure X11 keysym/button/modifier mapping has no Xlib dependency, so it runs on every platform.
add_executable(x11_input_translation_tests
    x11_input_translation_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_input_translation.cpp)

target_link_libraries(x11_input_translation_tests PRIVATE tessera_input)

target_include_directories(x11_input_translation_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/linux)

add_test(NAME x11_input_translation COMMAND x11_input_translation_tests)

add_executable(runtime_loop_tests
    runtime_loop_tests.cpp)

target_link_libraries(runtime_loop_tests PRIVATE tessera_runtime)

add_test(NAME runtime_loop COMMAND runtime_loop_tests)

add_executable(runtime_loop_async_tests
    runtime_loop_async_tests.cpp)

target_link_libraries(runtime_loop_async_tests PRIVATE tessera_runtime)

add_test(NAME runtime_loop_async COMMAND runtime_loop_async_tests)

add_executable(runtime_device_recovery_tests
    runtime_device_recovery_tests.cpp)

target_link_libraries(runtime_device_recovery_tests PRIVATE tessera_runtime)

add_test(NAME runtime_device_recovery COMMAND runtime_device_recovery_tests)

add_executable(clip_math_tests
    clip_math_tests.cpp)

target_link_libraries(clip_math_tests PRIVATE tessera_rhi)

add_test(NAME clip_math COMMAND clip_math_tests)

add_executable(diagnostics_crash_tests
    diagnostics_crash_tests.cpp)

target_link_libraries(diagnostics_crash_tests PRIVATE tessera_diagnostics)

add_test(NAME diagnostics_crash COMMAND diagnostics_crash_tests)

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

add_executable(memory_steady_state_tests
    memory_steady_state_tests.cpp)

target_link_libraries(memory_steady_state_tests PRIVATE tessera_diagnostics tessera_scene)

add_test(NAME memory_steady_state
    COMMAND memory_steady_state_tests
        ${PROJECT_SOURCE_DIR}/tests/golden/memory/steady_state.txt)

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

add_executable(damage_clear_tests
    damage_clear_tests.cpp)

target_link_libraries(damage_clear_tests PRIVATE tessera_damage)

add_test(NAME damage_clear COMMAND damage_clear_tests)

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

add_executable(style_var_tests
    style_var_tests.cpp)

target_link_libraries(style_var_tests PRIVATE tessera_style)

add_test(NAME style_var COMMAND style_var_tests)

add_executable(compiled_stylesheet_tests
    compiled_stylesheet_tests.cpp)

target_link_libraries(compiled_stylesheet_tests PRIVATE tessera_style)

add_test(NAME compiled_stylesheet COMMAND compiled_stylesheet_tests)

add_executable(stylesheet_blob_tests
    stylesheet_blob_tests.cpp)

target_link_libraries(stylesheet_blob_tests PRIVATE tessera_style)

add_test(NAME stylesheet_blob COMMAND stylesheet_blob_tests)

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

add_executable(animated_scalar_tests
    animated_scalar_tests.cpp)

target_link_libraries(animated_scalar_tests PRIVATE tessera_animation)

add_test(NAME animated_scalar COMMAND animated_scalar_tests)

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

add_executable(fallback_shaper_tests
    fallback_shaper_tests.cpp)

target_link_libraries(fallback_shaper_tests PRIVATE tessera_text)

add_test(NAME fallback_shaper COMMAND fallback_shaper_tests
    "${TESSERA_FONT_FIXTURE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/NotoSansHebrew-Regular.ttf")

add_executable(glyph_atlas_faces_tests
    glyph_atlas_faces_tests.cpp)

target_link_libraries(glyph_atlas_faces_tests PRIVATE tessera_text)

add_test(NAME glyph_atlas_faces COMMAND glyph_atlas_faces_tests
    "${TESSERA_FONT_FIXTURE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/NotoSansHebrew-Regular.ttf")

add_executable(glyph_color_tests
    glyph_color_tests.cpp)

target_link_libraries(glyph_color_tests PRIVATE tessera_text)

add_test(NAME glyph_color COMMAND glyph_color_tests
    "${TESSERA_FONT_FIXTURE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/ColorTest.ttf")

add_executable(emoji_font_tests
    emoji_font_tests.cpp)

target_link_libraries(emoji_font_tests PRIVATE tessera_text)

add_test(NAME emoji_font COMMAND emoji_font_tests
    "${TESSERA_FONT_FIXTURE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/Twemoji.Mozilla.ttf")

add_executable(text_layout_tests
    text_layout_tests.cpp)

target_link_libraries(text_layout_tests PRIVATE tessera_text)

add_test(NAME text_layout COMMAND text_layout_tests)

add_executable(text_paint_tests
    text_paint_tests.cpp)

target_link_libraries(text_paint_tests PRIVATE tessera_text)

add_test(NAME text_paint COMMAND text_paint_tests)

# CSS `word-spacing` (docs/design/text.md section 6.4): computed-value mapping, the separator
# counter, both measure paths, both cache keys, the paint advance sum, and determinism.
add_executable(word_spacing_tests
    word_spacing_tests.cpp)

target_link_libraries(word_spacing_tests PRIVATE tessera_paint tessera_text)

add_test(NAME word_spacing COMMAND word_spacing_tests)

# `text-overflow: ellipsis` (docs/design/text.md section 6.3): registry/cascade value, the fit rules
# at the text layer, layout invariance, and the end-to-end paint path.
add_executable(text_overflow_tests
    text_overflow_tests.cpp)

target_link_libraries(text_overflow_tests PRIVATE tessera_paint tessera_text)

add_test(NAME text_overflow COMMAND text_overflow_tests)

# CSS `text-transform` (docs/design/text.md section 6.5): the computed-value mapping, the shared
# ASCII 1:1 transform, the layout/measure seam, hit testing, both cache keys, and the end-to-end
# painted output. `text_wrap.hpp` is internal to the text module.
add_executable(text_transform_tests
    text_transform_tests.cpp)

target_link_libraries(text_transform_tests PRIVATE tessera_paint tessera_text)

target_include_directories(text_transform_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/ui/text)

add_test(NAME text_transform COMMAND text_transform_tests)

add_executable(text_bidi_tests
    text_bidi_tests.cpp)

target_link_libraries(text_bidi_tests PRIVATE tessera_text)

add_test(NAME text_bidi COMMAND text_bidi_tests)

add_executable(text_hit_test_tests
    text_hit_test_tests.cpp)

target_link_libraries(text_hit_test_tests PRIVATE tessera_text_edit)

add_test(NAME text_hit_test COMMAND text_hit_test_tests)

add_executable(paint_bidi_tests
    paint_bidi_tests.cpp)

target_link_libraries(paint_bidi_tests PRIVATE tessera_paint tessera_text)

add_test(NAME paint_bidi COMMAND paint_bidi_tests)

add_executable(paint_gradient_tests
    paint_gradient_tests.cpp)

target_link_libraries(paint_gradient_tests PRIVATE tessera_paint tessera_text)

add_test(NAME paint_gradient COMMAND paint_gradient_tests)

add_executable(text_edit_tests
    text_edit_tests.cpp)

target_link_libraries(text_edit_tests PRIVATE tessera_text)

add_test(NAME text_edit COMMAND text_edit_tests)

add_executable(edit_position_cache_tests
    edit_position_cache_tests.cpp)

target_link_libraries(edit_position_cache_tests PRIVATE tessera_text)

target_include_directories(edit_position_cache_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/ui/text)

add_test(NAME edit_position_cache COMMAND edit_position_cache_tests)

add_executable(text_edit_controller_tests
    text_edit_controller_tests.cpp)

target_link_libraries(text_edit_controller_tests PRIVATE tessera_text_edit)

add_test(NAME text_edit_controller COMMAND text_edit_controller_tests)
add_executable(text_input_runtime_tests
    text_input_runtime_tests.cpp)

target_link_libraries(text_input_runtime_tests PRIVATE tessera_text_edit)

add_test(NAME text_input_runtime COMMAND text_input_runtime_tests)
add_executable(caret_blink_tests
    caret_blink_tests.cpp)

target_link_libraries(caret_blink_tests PRIVATE tessera_text_edit)

add_test(NAME caret_blink COMMAND caret_blink_tests)
add_executable(text_edit_history_tests
    text_edit_history_tests.cpp)

target_link_libraries(text_edit_history_tests PRIVATE tessera_text_edit)

add_test(NAME text_edit_history COMMAND text_edit_history_tests)
add_executable(text_undo_tests
    text_undo_tests.cpp)

target_link_libraries(text_undo_tests PRIVATE tessera_text_edit)

add_test(NAME text_undo COMMAND text_undo_tests)

add_executable(text_edit_clipboard_tests
    text_edit_clipboard_tests.cpp)

target_link_libraries(text_edit_clipboard_tests PRIVATE tessera_text_edit tessera_hal_clipboard)

add_test(NAME text_edit_clipboard COMMAND text_edit_clipboard_tests)

add_executable(layout_apply_tests
    layout_apply_tests.cpp)

target_link_libraries(layout_apply_tests PRIVATE tessera_layout)

add_test(NAME layout_apply COMMAND layout_apply_tests)

add_executable(layout_observer_tests
    layout_observer_tests.cpp)

target_link_libraries(layout_observer_tests PRIVATE tessera_layout)

add_test(NAME layout_observer COMMAND layout_observer_tests)

add_executable(layout_observer_settle_tests
    layout_observer_settle_tests.cpp)

target_link_libraries(layout_observer_settle_tests PRIVATE tessera_layout)

add_test(NAME layout_observer_settle COMMAND layout_observer_settle_tests)

add_executable(layout_read_model_tests
    layout_read_model_tests.cpp)

target_link_libraries(layout_read_model_tests PRIVATE tessera_layout)

add_test(NAME layout_read_model COMMAND layout_read_model_tests)

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
add_executable(frame_pipeline_overflow_tests
    frame_pipeline_overflow_tests.cpp)

target_link_libraries(frame_pipeline_overflow_tests PRIVATE tessera_frame tessera_runtime)

add_test(NAME frame_pipeline_overflow COMMAND frame_pipeline_overflow_tests)

add_executable(frame_pipeline_style_sync_tests
    frame_pipeline_style_sync_tests.cpp)

target_link_libraries(frame_pipeline_style_sync_tests PRIVATE tessera_frame tessera_runtime)

add_test(NAME frame_pipeline_style_sync COMMAND frame_pipeline_style_sync_tests)

add_executable(lowering_expression_tests
    lowering_expression_tests.cpp)

target_link_libraries(lowering_expression_tests PRIVATE tessera_lowering)

add_test(NAME lowering_expression COMMAND lowering_expression_tests)

add_executable(lowering_reject_tests
    lowering_reject_tests.cpp)

target_link_libraries(lowering_reject_tests PRIVATE tessera_lowering)

add_test(NAME lowering_reject COMMAND lowering_reject_tests)

add_executable(lowering_span_tests
    lowering_span_tests.cpp)

target_link_libraries(lowering_span_tests PRIVATE tessera_lowering)

add_test(NAME lowering_span COMMAND lowering_span_tests)

add_executable(lowering_determinism_tests
    lowering_determinism_tests.cpp)

target_link_libraries(lowering_determinism_tests PRIVATE tessera_lowering)

add_test(NAME lowering_determinism COMMAND lowering_determinism_tests)

add_executable(lowering_golden_tests
    lowering_golden_tests.cpp)

target_link_libraries(lowering_golden_tests PRIVATE tessera_lowering)

target_compile_definitions(lowering_golden_tests PRIVATE
    TESSERA_LOWERING_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/lowering")

add_test(NAME lowering_golden COMMAND lowering_golden_tests)

add_executable(lowering_props_tests
    lowering_props_tests.cpp)

target_link_libraries(lowering_props_tests PRIVATE tessera_lowering)

add_test(NAME lowering_props COMMAND lowering_props_tests)

add_executable(lowering_layering_tests
    lowering_layering_tests.cpp)

target_link_libraries(lowering_layering_tests PRIVATE tessera_lowering)

target_compile_definitions(lowering_layering_tests PRIVATE
    TESSERA_COMPILER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/compiler")

add_test(NAME lowering_layering COMMAND lowering_layering_tests)

add_executable(lowering_hooks_tests
    lowering_hooks_tests.cpp)

target_link_libraries(lowering_hooks_tests PRIVATE tessera_lowering)

add_test(NAME lowering_hooks COMMAND lowering_hooks_tests)

add_executable(lowering_hooks_reject_tests
    lowering_hooks_reject_tests.cpp)

target_link_libraries(lowering_hooks_reject_tests PRIVATE tessera_lowering)

add_test(NAME lowering_hooks_reject COMMAND lowering_hooks_reject_tests)

add_executable(lowering_hooks_determinism_tests
    lowering_hooks_determinism_tests.cpp)

target_link_libraries(lowering_hooks_determinism_tests PRIVATE
    tessera_lowering
    Threads::Threads)

add_test(NAME lowering_hooks_determinism COMMAND lowering_hooks_determinism_tests)

add_executable(lowering_nodes_tests
    lowering_nodes_tests.cpp)

target_link_libraries(lowering_nodes_tests PRIVATE tessera_lowering)

add_test(NAME lowering_nodes COMMAND lowering_nodes_tests)

add_executable(lowering_nodes_reject_tests
    lowering_nodes_reject_tests.cpp)

target_link_libraries(lowering_nodes_reject_tests PRIVATE tessera_lowering)

add_test(NAME lowering_nodes_reject COMMAND lowering_nodes_reject_tests)

add_executable(lowering_nodes_determinism_tests
    lowering_nodes_determinism_tests.cpp)

target_link_libraries(lowering_nodes_determinism_tests PRIVATE
    tessera_lowering
    Threads::Threads)

add_test(NAME lowering_nodes_determinism COMMAND lowering_nodes_determinism_tests)

add_executable(lowering_control_tests
    lowering_control_tests.cpp)

target_link_libraries(lowering_control_tests PRIVATE tessera_lowering)

add_test(NAME lowering_control COMMAND lowering_control_tests)

add_executable(lowering_control_reject_tests
    lowering_control_reject_tests.cpp)

target_link_libraries(lowering_control_reject_tests PRIVATE tessera_lowering)

add_test(NAME lowering_control_reject COMMAND lowering_control_reject_tests)

add_executable(lowering_control_determinism_tests
    lowering_control_determinism_tests.cpp)

target_link_libraries(lowering_control_determinism_tests PRIVATE
    tessera_lowering
    Threads::Threads)

add_test(NAME lowering_control_determinism COMMAND lowering_control_determinism_tests)

add_executable(lowering_module_tests
    lowering_module_tests.cpp)

target_link_libraries(lowering_module_tests PRIVATE tessera_lowering)

add_test(NAME lowering_module COMMAND lowering_module_tests)

add_executable(lowering_module_package_tests
    lowering_module_package_tests.cpp)

target_link_libraries(lowering_module_package_tests PRIVATE tessera_lowering)

add_test(NAME lowering_module_package COMMAND lowering_module_package_tests)

add_executable(lowering_module_reject_tests
    lowering_module_reject_tests.cpp)

target_link_libraries(lowering_module_reject_tests PRIVATE tessera_lowering)

add_test(NAME lowering_module_reject COMMAND lowering_module_reject_tests)

add_executable(lowering_module_stylesheet_tests
    lowering_module_stylesheet_tests.cpp)

target_link_libraries(lowering_module_stylesheet_tests PRIVATE tessera_lowering tessera_codegen)

add_test(NAME lowering_module_stylesheet COMMAND lowering_module_stylesheet_tests)

add_executable(lowering_module_determinism_tests
    lowering_module_determinism_tests.cpp)

target_link_libraries(lowering_module_determinism_tests PRIVATE
    tessera_lowering
    Threads::Threads)

add_test(NAME lowering_module_determinism COMMAND lowering_module_determinism_tests)

# Field/Form are authored as real TSX sources (issue #70): the build lowers the two files under
# tessera/components/ into library_components_generated.cpp, and this test re-lowers the committed
# sources in-process to prove the embedded declaration is not stale and cannot drift.
add_executable(library_tsx_parity_tests
    library_tsx_parity_tests.cpp)

target_link_libraries(library_tsx_parity_tests PRIVATE tessera_lowering)

target_compile_definitions(library_tsx_parity_tests PRIVATE
    TESSERA_LIBRARY_TSX_DIR="${CMAKE_SOURCE_DIR}/tessera/components")

add_test(NAME library_tsx_parity COMMAND library_tsx_parity_tests)

add_executable(paint_pass_tests
    paint_pass_tests.cpp)

target_link_libraries(paint_pass_tests PRIVATE
    tessera_paint
    tessera_rhi_null)

add_test(NAME paint_pass COMMAND paint_pass_tests)

add_executable(z_order_tests
    z_order_tests.cpp)

target_link_libraries(z_order_tests PRIVATE tessera_paint)

add_test(NAME z_order COMMAND z_order_tests)

add_executable(transform_tests
    transform_tests.cpp)

target_link_libraries(transform_tests PRIVATE tessera_style)

add_test(NAME transform COMMAND transform_tests)

add_executable(paint_shadow_tests
    paint_shadow_tests.cpp)

target_include_directories(paint_shadow_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/ui/paint)
target_link_libraries(paint_shadow_tests PRIVATE tessera_paint)

add_test(NAME paint_shadow COMMAND paint_shadow_tests)

add_executable(paint_transform_tests
    paint_transform_tests.cpp)

target_link_libraries(paint_transform_tests PRIVATE tessera_paint)

add_test(NAME paint_transform COMMAND paint_transform_tests)

add_executable(scene_style_sync_tests
    scene_style_sync_tests.cpp)

target_link_libraries(scene_style_sync_tests PRIVATE tessera_paint tessera_layout)

add_test(NAME scene_style_sync COMMAND scene_style_sync_tests)

add_executable(paint_clip_tests
    paint_clip_tests.cpp)

target_link_libraries(paint_clip_tests PRIVATE tessera_paint)

add_test(NAME paint_clip COMMAND paint_clip_tests)

add_executable(paint_text_tests
    paint_text_tests.cpp)

target_link_libraries(paint_text_tests PRIVATE tessera_paint tessera_frame tessera_runtime)

add_test(NAME paint_text COMMAND paint_text_tests)

add_executable(paint_text_wrap_tests
    paint_text_wrap_tests.cpp)

target_link_libraries(paint_text_wrap_tests PRIVATE tessera_paint tessera_frame tessera_runtime)

add_test(NAME paint_text_wrap COMMAND paint_text_wrap_tests)

add_executable(cpu_canvas_tests
    cpu_canvas_tests.cpp)

target_link_libraries(cpu_canvas_tests PRIVATE tessera_software)

add_test(NAME cpu_canvas COMMAND cpu_canvas_tests)

# Deterministic CPU-canvas golden corpus: every primitive the software backend implements (solid
# quads, coverage blend, textured quads) plus boundary geometry, asserted on exact RGBA8 bytes. The
# second executable covers the sink-level primitives (clip stacks, translate/rotation, rounded
# bounds, damage-scissored replays). No GPU, lavapipe, font, clock, or network.
add_executable(cpu_canvas_corpus_tests
    cpu_canvas_corpus_tests.cpp)

target_link_libraries(cpu_canvas_corpus_tests PRIVATE tessera_software)

add_test(NAME cpu_canvas_corpus COMMAND cpu_canvas_corpus_tests)

add_executable(cpu_canvas_scene_corpus_tests
    cpu_canvas_scene_corpus_tests.cpp)

target_link_libraries(cpu_canvas_scene_corpus_tests PRIVATE
    tessera_paint
    tessera_software)

add_test(NAME cpu_canvas_scene_corpus COMMAND cpu_canvas_scene_corpus_tests)

# Vulkan-free dirty-only == full-redraw equivalence: damage plan -> damage-clipped paint ->
# deterministic CPU canvas pixels. Runs on every platform; no GPU/lavapipe required.
add_executable(damage_equivalence_cpu_tests
    damage_equivalence_cpu_tests.cpp)
target_link_libraries(damage_equivalence_cpu_tests PRIVATE
    tessera_frame
    tessera_paint
    tessera_software
    tessera_runtime)
add_test(NAME damage_equivalence_cpu COMMAND damage_equivalence_cpu_tests)

add_executable(paint_text_layout_cache_tests
    paint_text_layout_cache_tests.cpp)

target_link_libraries(paint_text_layout_cache_tests PRIVATE tessera_paint tessera_text)

add_test(NAME paint_text_layout_cache COMMAND paint_text_layout_cache_tests)

add_executable(paint_text_decoration_tests
    paint_text_decoration_tests.cpp)

target_link_libraries(paint_text_decoration_tests PRIVATE
    tessera_paint tessera_frame tessera_runtime)

add_test(NAME paint_text_decoration COMMAND paint_text_decoration_tests)
add_executable(paint_text_preedit_tests
    paint_text_preedit_tests.cpp)

target_link_libraries(paint_text_preedit_tests PRIVATE
    tessera_paint tessera_text_edit)

add_test(NAME paint_text_preedit COMMAND paint_text_preedit_tests)

add_executable(forms_model_tests
    forms_model_tests.cpp)

target_link_libraries(forms_model_tests PRIVATE tessera_forms)

add_test(NAME forms_model COMMAND forms_model_tests)

add_executable(forms_async_tests
    forms_async_tests.cpp)

target_link_libraries(forms_async_tests PRIVATE tessera_forms tessera_runtime)

add_test(NAME forms_async COMMAND forms_async_tests)

add_executable(input_pipeline_tests
    input_pipeline_tests.cpp)

target_link_libraries(input_pipeline_tests PRIVATE tessera_input_pump)

add_test(NAME input_pipeline COMMAND input_pipeline_tests)

add_executable(input_hover_tests
    input_hover_tests.cpp)

target_link_libraries(input_hover_tests PRIVATE tessera_scene)

add_test(NAME input_hover COMMAND input_hover_tests)

add_executable(input_text_tests
    input_text_tests.cpp)

target_link_libraries(input_text_tests PRIVATE tessera_scene)

add_test(NAME input_text COMMAND input_text_tests)

add_executable(input_accelerator_tests
    input_accelerator_tests.cpp)

target_link_libraries(input_accelerator_tests PRIVATE tessera_scene)

add_test(NAME input_accelerator COMMAND input_accelerator_tests)

add_executable(scroll_tests
    scroll_tests.cpp)

target_link_libraries(scroll_tests PRIVATE tessera_scene tessera_paint tessera_layout)

add_test(NAME scroll_core COMMAND scroll_tests)

add_executable(scroll_pan_tests
    scroll_pan_tests.cpp)

target_link_libraries(scroll_pan_tests PRIVATE tessera_input_pump)

add_test(NAME scroll_pan COMMAND scroll_pan_tests)

add_executable(scroll_focus_tests
    scroll_focus_tests.cpp)

target_link_libraries(scroll_focus_tests PRIVATE tessera_scene)

add_test(NAME scroll_focus COMMAND scroll_focus_tests)

add_executable(scroll_insets_tests
    scroll_insets_tests.cpp)

target_link_libraries(scroll_insets_tests PRIVATE tessera_scene tessera_paint)

add_test(NAME scroll_insets COMMAND scroll_insets_tests)

add_executable(a11y_tree_tests
    a11y_tree_tests.cpp
    a11y_semantics_tests.cpp)

target_link_libraries(a11y_tree_tests PRIVATE tessera_accessibility)

add_test(NAME a11y_tree COMMAND a11y_tree_tests)

add_executable(schema_tests
    schema_tests.cpp)

target_link_libraries(schema_tests PRIVATE tessera_schema)

add_test(NAME schema_table COMMAND schema_tests)

add_executable(schema_layering_tests
    schema_layering_tests.cpp)

target_link_libraries(schema_layering_tests PRIVATE tessera_schema)

target_compile_definitions(schema_layering_tests PRIVATE
    TESSERA_SCHEMA_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/schema")

add_test(NAME schema_layering COMMAND schema_layering_tests)

add_executable(schema_golden_tests
    schema_golden_tests.cpp)

target_link_libraries(schema_golden_tests PRIVATE tessera_schema_dump_core)

target_compile_definitions(schema_golden_tests PRIVATE
    TESSERA_SCHEMA_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden/schema")

add_test(NAME schema_golden COMMAND schema_golden_tests)

add_executable(gesture_arena_tests
    gesture_arena_tests.cpp)

target_link_libraries(gesture_arena_tests PRIVATE tessera_input)

add_test(NAME gesture_arena COMMAND gesture_arena_tests)

add_executable(gesture_recognizer_tests
    gesture_recognizer_tests.cpp)

target_link_libraries(gesture_recognizer_tests PRIVATE tessera_input)

add_test(NAME gesture_recognizers COMMAND gesture_recognizer_tests)

add_executable(gesture_pipeline_tests
    gesture_pipeline_tests.cpp)

target_link_libraries(gesture_pipeline_tests PRIVATE tessera_input_pump)

add_test(NAME gesture_pipeline COMMAND gesture_pipeline_tests)

# Headless generated-UI integration demo (task-56). The canonical target definition lives in
# tests/integration/CMakeLists.txt; this include is the build fallback until the root CMakeLists
# (Lead-owned) adds tests/integration. Both paths are guarded to stay idempotent.
include(${PROJECT_SOURCE_DIR}/tests/integration/CMakeLists.txt)
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

add_executable(layout_scheduler_tests
    layout_scheduler_tests.cpp)

target_link_libraries(layout_scheduler_tests PRIVATE tessera_layout)

add_test(NAME layout_scheduler COMMAND layout_scheduler_tests)

add_executable(layout_signature_tests
    layout_signature_tests.cpp)

target_link_libraries(layout_signature_tests PRIVATE tessera_layout)

add_test(NAME layout_signature COMMAND layout_signature_tests)

add_executable(swapchain_tests
    swapchain_tests.cpp)

target_link_libraries(swapchain_tests PRIVATE tessera_rhi_null)

add_test(NAME swapchain COMMAND swapchain_tests)

add_executable(frame_wait_tests
    frame_wait_tests.cpp)

target_link_libraries(frame_wait_tests PRIVATE tessera_runtime)

add_test(NAME frame_wait COMMAND frame_wait_tests)

add_executable(io_executor_tests
    io_executor_tests.cpp)

target_link_libraries(io_executor_tests PRIVATE tessera_runtime)

add_test(NAME io_executor COMMAND io_executor_tests)

# Each subsystem below keeps its test registration in its own folder so two slices never edit this
# list at the same time; add_subdirectory stays here so the list remains the single index.
add_subdirectory(automation)
add_subdirectory(virtual)
add_subdirectory(boundary)
add_subdirectory(a11y)
add_subdirectory(harness)
add_subdirectory(manifest)
add_subdirectory(scale)

# ICU-backed text tests exist only when the reduced-data option is on (CI keeps it off).
if (TESSERA_ICU_REDUCED_DATA)
    add_subdirectory(icu)
endif()

add_executable(container_query_tests
    container_query_tests.cpp)

target_link_libraries(container_query_tests PRIVATE tessera_style)

add_test(NAME container_query COMMAND container_query_tests)

add_executable(container_query_settle_tests
    container_query_settle_tests.cpp)

target_link_libraries(container_query_settle_tests PRIVATE tessera_layout)

add_test(NAME container_query_settle COMMAND container_query_settle_tests)

add_executable(tofu_placeholder_tests
    tofu_placeholder_tests.cpp)

target_link_libraries(tofu_placeholder_tests PRIVATE tessera_text)

add_test(NAME tofu_placeholder COMMAND tofu_placeholder_tests
    "${TESSERA_FONT_FIXTURE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/EmptyNotdef.ttf")

# IR expression and action evaluator (docs/specs/ir-v1.md sections 4.1 and 4.2).
add_executable(ir_eval_tests
    ir_eval_tests.cpp)

target_link_libraries(ir_eval_tests PRIVATE tessera_ir_eval)

add_test(NAME ir_eval COMMAND ir_eval_tests)

# StateCell helpers behind generated snapshot_state() (issue #7).
add_executable(state_snapshot_tests
    state_snapshot_tests.cpp)

target_include_directories(state_snapshot_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_test(NAME state_snapshot COMMAND state_snapshot_tests)

# Numeric identity assignment for the IR (docs/specs/automation-ids-v1.md sections 2-3).
add_executable(ir_ids_tests
    ir_ids_tests.cpp)

target_link_libraries(ir_ids_tests PRIVATE tessera_ir_ids)

add_test(NAME ir_ids COMMAND ir_ids_tests)

add_executable(animation_event_tests
    animation_event_tests.cpp)

target_link_libraries(animation_event_tests PRIVATE tessera_animation)

add_test(NAME animation_event COMMAND animation_event_tests)

add_executable(frame_pipeline_spring_tests
    frame_pipeline_spring_tests.cpp)

target_link_libraries(frame_pipeline_spring_tests PRIVATE tessera_frame tessera_runtime)

add_test(NAME frame_pipeline_spring COMMAND frame_pipeline_spring_tests)

add_executable(webgpu_mapping_tests
    webgpu_mapping_tests.cpp)

target_link_libraries(webgpu_mapping_tests PRIVATE tessera_webgpu_mapping)

add_test(NAME webgpu_mapping COMMAND webgpu_mapping_tests)

# The FramePipeline/AppHost-owned per-node paint cache (docs/design/dirty-rendering.md 5.4): a
# settled frame replays its records, output stays byte-identical to the uncached pass, and a
# style/tree generation change invalidates instead of replaying the previous generation.
add_executable(frame_paint_cache_tests
    frame_paint_cache_tests.cpp)

target_link_libraries(frame_paint_cache_tests PRIVATE
    tessera_app
    tessera_frame
    tessera_runtime)

add_test(NAME frame_paint_cache COMMAND frame_paint_cache_tests)
