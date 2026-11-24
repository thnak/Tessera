# SPDX-License-Identifier: MIT

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

# ADR-066 item 1: the React spellings a port uses (arrow components, `import React`, `useId`, the
# safe `useMemo`/`useCallback` forms, `??`/`?.`). Split from jsx_module_tests when that file reached
# the 500-line hard limit.
add_executable(jsx_react_core_tests
    jsx_react_core_tests.cpp)

target_link_libraries(jsx_react_core_tests PRIVATE tessera_jsx_frontend)

add_test(NAME jsx_react_core COMMAND jsx_react_core_tests)

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

# The corpus measurement golden (spike #660, ADR-066): the front end and the pipeline over the
# in-repo React fixtures, compared byte for byte with tests/golden/corpus/react-front-end.txt. The
# corpus is written in this repository, so the measurement needs no network and no downloaded code.
add_executable(corpus_measure_tests
    corpus_measure_tests.cpp)

target_link_libraries(corpus_measure_tests PRIVATE tessera_corpus_measure_core)

target_compile_definitions(corpus_measure_tests PRIVATE
    TESSERA_CORPUS_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/corpus/react"
    TESSERA_CORPUS_GOLDEN="${CMAKE_SOURCE_DIR}/tests/golden/corpus/react-front-end.txt")

add_test(NAME corpus_measure COMMAND corpus_measure_tests)

add_executable(ir_authored_class_tests
    ir_authored_class_tests.cpp)

target_link_libraries(ir_authored_class_tests PRIVATE tessera_ir)

add_test(NAME ir_authored_class COMMAND ir_authored_class_tests)

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

# Per-kind traits table (god-type audit section 4.3). The table's alignment is a compile-time
# property (src/compiler/ir/src/node_kind_traits.cpp); this test proves it agrees with the rest of
# the IR -- `node_kind_name` for every kind, and the field groups the validator reports from.
add_executable(node_kind_traits_tests
    node_kind_traits_tests.cpp)
target_link_libraries(node_kind_traits_tests PRIVATE
    tessera_ir)

add_test(NAME node_kind_traits COMMAND node_kind_traits_tests)

# Issue #158 / ADR-051: IR record types and record values (docs/specs/ir-v1.md sections 2.2, 4.3 and
# 9). Two translation units: the writer (`record` expression, optional `types` array, and the
# byte-identity of a module with no record) and the validator (type table, cycles, field rules, the
# three allowed positions, dotted refs, the unused warning).
add_executable(ir_record_writer_tests
    ir_record_writer_tests.cpp
    ir_samples.cpp
    ir_samples_rich.cpp
    ir_samples_valid.cpp
    ir_samples_record.cpp)

target_link_libraries(ir_record_writer_tests PRIVATE tessera_ir)

add_test(NAME ir_record_writer COMMAND ir_record_writer_tests)

add_executable(ir_record_table_tests
    ir_record_table_tests.cpp
    ir_samples.cpp
    ir_samples_rich.cpp
    ir_samples_valid.cpp
    ir_samples_record.cpp)

target_link_libraries(ir_record_table_tests PRIVATE tessera_ir)

add_test(NAME ir_record_table COMMAND ir_record_table_tests)

add_executable(ir_record_value_tests
    ir_record_value_tests.cpp
    ir_samples.cpp
    ir_samples_rich.cpp
    ir_samples_valid.cpp
    ir_samples_record.cpp)

target_link_libraries(ir_record_value_tests PRIVATE tessera_ir)

add_test(NAME ir_record_value COMMAND ir_record_value_tests)

add_executable(ir_layering_tests
    ir_layering_tests.cpp)

target_compile_definitions(ir_layering_tests PRIVATE
    TESSERA_COMPILER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/compiler")

add_test(NAME ir_layering COMMAND ir_layering_tests)

add_executable(rules_hooks_tests
    rules_hooks_tests.cpp)

target_link_libraries(rules_hooks_tests PRIVATE tessera_react_rules)

add_test(NAME rules_hooks COMMAND rules_hooks_tests)

# ADR-066 item 4: the `useReducer` form, which is defined by a module-scope helper and the type
# aliases beside it rather than by the component body alone.
add_executable(rules_reducer_tests
    rules_reducer_tests.cpp)

target_link_libraries(rules_reducer_tests PRIVATE tessera_react_rules)

add_test(NAME rules_reducer COMMAND rules_reducer_tests)

add_executable(rules_arrows_tests
    rules_arrows_tests.cpp)

target_link_libraries(rules_arrows_tests PRIVATE tessera_react_rules)

add_test(NAME rules_arrows COMMAND rules_arrows_tests)

add_executable(rules_effect_deps_tests
    rules_effect_deps_tests.cpp)

target_link_libraries(rules_effect_deps_tests PRIVATE tessera_react_rules)

add_test(NAME rules_effect_deps COMMAND rules_effect_deps_tests)

# Issue #829: `useMemo`/`useCallback` dependency lists are compared with the inferred set and warn
# on a mismatch (never an error), unlike the exact `useEffect` rule above.
add_executable(rules_memo_deps_tests
    rules_memo_deps_tests.cpp)

target_link_libraries(rules_memo_deps_tests PRIVATE tessera_react_rules)

add_test(NAME rules_memo_deps COMMAND rules_memo_deps_tests)

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

# hit_test_deep maps the point one level at a time and prunes by the cached subtree paint box. A
# differential test against the unpruned algorithm over random trees, plus the cases pruning could
# get wrong (children outside the parent, shadows, unbounded text, transforms, a stale cache).
add_executable(scene_hit_test_prune_tests
    scene_hit_test_prune_tests.cpp)

target_link_libraries(scene_hit_test_prune_tests PRIVATE tessera_scene)

add_test(NAME scene_hit_test_prune COMMAND scene_hit_test_prune_tests)

# Node::clear_subtree_dirty (DirtyPass::clear) must leave every node as the per-node clear_dirty walk
# it replaced did, on the cleared subtree and on its ancestors.
add_executable(scene_dirty_clear_tests
    scene_dirty_clear_tests.cpp)

target_link_libraries(scene_dirty_clear_tests PRIVATE tessera_scene)

add_test(NAME scene_dirty_clear COMMAND scene_dirty_clear_tests)

# The small value types Node is composed of: each owns its values and reports "changed".
add_executable(node_parts_tests
    node_parts_tests.cpp)

target_link_libraries(node_parts_tests PRIVATE tessera_scene)

add_test(NAME node_parts COMMAND node_parts_tests)

# The sample framework-linking executable of the release gates (ADR-012, issue #219): it replaces
# the deleted `merger_tests` as the target `check_static_runtime.py`, `check_reproducible.py` and
# `release_rehearsal.py` build and inspect. Registered as a test too, so a broken probe fails the
# normal suite instead of only the release lanes.
add_executable(static_runtime_probe
    static_runtime_probe.cpp)

target_link_libraries(static_runtime_probe PRIVATE tessera_runtime tessera_scene tessera_style)

add_test(NAME static_runtime_probe_links COMMAND static_runtime_probe)

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

# Focus scope (trap and restore) and roving focus (issue #236), headless through the InputRouter.
add_executable(focus_scope_tests
    focus_scope_tests.cpp)

target_link_libraries(focus_scope_tests PRIVATE tessera_scene)

add_test(NAME focus_scope COMMAND focus_scope_tests)

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

# A host built over an ALREADY SETTLED tree must still paint its first frame (the Android fold
# defect, docs/guides/android-lane.md section 3.19). Destroying and recreating the surface rebuilds
# the host around the shell's live scene, whose nodes are clean, so a damage-only first frame planned
# nothing and the window stayed black. Headless: ManualClock, a counting paint sink.
add_executable(app_host_repaint_tests
    app_host_repaint_tests.cpp)

target_link_libraries(app_host_repaint_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME app_host_repaint COMMAND app_host_repaint_tests)

# The ADR-041 settle loop wired into a real AppHost (issue #693): a `@container` rule that crosses a
# condition re-styles in the same tick as the resize, a resize inside one condition costs no engine
# round, a stylesheet without `@container` pays nothing, and the round budget terminates a
# self-sizing rule instead of hanging the frame. Headless: ManualClock, null paint sink.
add_executable(layout_settle_host_tests
    layout_settle_host_tests.cpp)

target_link_libraries(layout_settle_host_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME layout_settle_host COMMAND layout_settle_host_tests)

# The layout-unchanged gate's acceptance (issue #735, slice A): a frame whose layout pass the gate
# skipped must be byte-identical to a cold layout of the same inputs, over a randomized edit
# sequence that covers every layout input source (longhands, text, shape, viewport).
add_executable(layout_gate_tests
    layout_gate_tests.cpp
    layout_gate_scene.cpp)

target_link_libraries(layout_gate_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME layout_gate COMMAND layout_gate_tests)

# The scoped scene sync (issue #735, slice B): a walk that visits only the restyled subtrees must
# leave the scene in exactly the state the full walk does, including the `glass_subtree` refold on
# every ancestor.
add_executable(scoped_sync_tests
    scoped_sync_tests.cpp)

target_link_libraries(scoped_sync_tests PRIVATE tessera_paint tessera_style)

add_test(NAME scoped_sync COMMAND scoped_sync_tests)

# The host half of issue #187's acceptance: "the host updates reducedMotion and the media queries".
# It drives the real AppHost with a media-independent prepared sheet (ADR-048's resolve step) and
# asserts both `reducedMotion` reaching the animation/spring runtimes and the
# `prefers-color-scheme`/`prefers-reduced-motion` evaluation moving with `set_appearance`, plus the
# fake window's normalized ThemeChanged/SystemSettingsChanged seam. Deterministic: ManualClock,
# ManualExecutor-free, no bus, no desktop.
add_executable(app_host_appearance_tests
    app_host_appearance_tests.cpp)

target_link_libraries(app_host_appearance_tests PRIVATE
    tessera_app
    tessera_runtime
    tessera_harness_testing)

add_test(NAME app_host_appearance COMMAND app_host_appearance_tests)

# Embedded stylesheet registration (issue #81, ADR-048): blobs load in table (import-graph) order,
# a later sheet wins an equal cascade key, and a corrupt blob is reported and skipped. Painting goes
# through a deterministic CPU sink; no GPU, font or wall clock.
add_executable(app_host_stylesheet_tests
    app_host_stylesheet_tests.cpp)

target_link_libraries(app_host_stylesheet_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME app_host_stylesheet COMMAND app_host_stylesheet_tests)

# Embedded theme asset registration (issue #81, ADR-048): the host loads the generated blob, injects
# only the active theme's tokens, switches themes and rejects a corrupt blob without losing the
# current theme. Painting goes through a deterministic CPU sink; no GPU, font or wall clock.
add_executable(app_host_theme_tests
    app_host_theme_tests.cpp)

target_link_libraries(app_host_theme_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME app_host_theme COMMAND app_host_theme_tests)

# The window driver's runtime-config mapping (docs/specs/runtime-config-v1.md section 6.2): the two
# wired keys map onto the frame pipeline's interpolation switch and the occluded/minimized pause
# policy, the mapped defaults match RuntimeConfig{} field by field, and the mapped pipeline config is
# driven through the paint seam. Pure CPU: no GPU, window or font.
add_executable(window_config_tests
    window_config_tests.cpp)

target_include_directories(window_config_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app)

target_link_libraries(window_config_tests PRIVATE tessera_app tessera_frame tessera_runtime)

add_test(NAME window_config COMMAND window_config_tests)

# The window's display-aware AA policy mapping (docs/design/display-aware-rendering.md sections 2, 5
# and 6): window metrics plus the renderer inputs resolve to an AaPolicy, the explicit switch and a
# golden/AA-off renderer pin `golden_aa_policy()`, and the power preset and MSAA rules reach the
# resolver. Pure CPU: no GPU, window or display.
add_executable(window_aa_policy_tests
    window_aa_policy_tests.cpp)

target_include_directories(window_aa_policy_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app)

target_link_libraries(window_aa_policy_tests PRIVATE tessera_app)

add_test(NAME window_aa_policy COMMAND window_aa_policy_tests)

# Adaptive quality (docs/design/performance.md section 6; runtime-config-v1.md 5.7 and 6.2): the
# deterministic controller's thresholds, clamps, disabled paths, hysteresis and determinism, plus the
# host wiring that feeds it one observed frame at a time and records a change - not a raw frame time
# - in frame diagnostics. Pure CPU: no GPU, window or wall clock.
add_executable(adaptive_quality_tests
    adaptive_quality_tests.cpp)

target_include_directories(adaptive_quality_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/runtime)

target_link_libraries(adaptive_quality_tests PRIVATE
    tessera_app
    tessera_runtime)

add_test(NAME adaptive_quality COMMAND adaptive_quality_tests)

# The adaptive-quality stage of AppHost (god-type audit slice 10): the frame-time selection, its
# diagnostics recording, and the two application rules a decision implies -- a reduced-effects flip
# requests exactly one full repaint, a paused-animations flip marks the loop dirty exactly once.
# Driven with a fake context, so no host, window or GPU.
add_executable(app_quality_stage_tests
    app_quality_stage_tests.cpp)

target_include_directories(app_quality_stage_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app
    ${PROJECT_SOURCE_DIR}/src/runtime)

target_link_libraries(app_quality_stage_tests PRIVATE
    tessera_app
    tessera_runtime)

add_test(NAME app_quality_stage COMMAND app_quality_stage_tests)

# GPU frame-time signal (docs/design/performance.md section 6.1): the timestamp interval arithmetic
# (period, 64-bit wraparound, zero/absurd delta and unusable period), the capability gate with a fake
# probe (usable -> GPU, any missing fact -> CPU), and the host wiring that feeds the controller the
# GPU interval when present and the CPU time otherwise while recording which source ran. Pure CPU:
# no GPU, window or wall clock. The real Vulkan query pair needs a device and is unverified here.
add_executable(gpu_frame_time_tests
    gpu_frame_time_tests.cpp)

target_include_directories(gpu_frame_time_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/runtime)

target_link_libraries(gpu_frame_time_tests PRIVATE
    tessera_app
    tessera_runtime)

add_test(NAME gpu_frame_time COMMAND gpu_frame_time_tests)

# The two WindowHost::Impl state clusters extracted by the god-type audit slice 9: the GPU frame-time
# probe (capability gate, source selection, counted fallbacks, reset across a device loss) and the
# adaptive render-scale targets (one per slot, reuse while the extent lasts, release per generation).
# Pure CPU with the null backend: no window, no wall clock.
add_executable(window_state_clusters_tests
    window_state_clusters_tests.cpp)

target_include_directories(window_state_clusters_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app)

target_link_libraries(window_state_clusters_tests PRIVATE
    tessera_app
    tessera_rhi_null
    tessera_runtime)

add_test(NAME window_state_clusters COMMAND window_state_clusters_tests)

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

# Replaces the global operator new/delete to count allocations; see the helper's comment.
tessera_allow_allocator_override(node_size_tests)

add_test(NAME node_size COMMAND node_size_tests)

# The shared text helpers (issue #733): header-only, so this target exists to compile and exercise
# them on every lane. It links `tessera_style` only to inherit the public include path; the header
# itself has no dependency.
add_executable(text_util_tests
    text_util_tests.cpp)

target_link_libraries(text_util_tests PRIVATE tessera_style)

add_test(NAME text_util COMMAND text_util_tests)

# The paint cache's per-op size budget (issue #731): the assertions are compile-time, so this target
# exists to compile them on every lane.
add_executable(paint_op_size_tests
    paint_op_size_tests.cpp)

target_link_libraries(paint_op_size_tests PRIVATE tessera_paint)

add_test(NAME paint_op_size COMMAND paint_op_size_tests)

# The resolved background-image value's size budget (issue #731): the assertions are compile-time, so
# this target exists to compile them on every lane.
add_executable(background_layer_size_tests
    background_layer_size_tests.cpp)

target_link_libraries(background_layer_size_tests PRIVATE tessera_style)

add_test(NAME background_layer_size COMMAND background_layer_size_tests)

# The internal-padding budget of issue #731: the assertions are compile-time, so this target exists
# to compile them on every lane.
add_executable(padding_size_tests
    padding_size_tests.cpp)

target_link_libraries(padding_size_tests PRIVATE tessera_runtime tessera_scene)

add_test(NAME padding_size COMMAND padding_size_tests)

# Cache-line layout invariants (security review findings C1, C2, C3, C7): the assertions are
# compile-time, so this target exists to compile them on every lane.
add_executable(cache_line_tests
    cache_line_tests.cpp)

target_link_libraries(cache_line_tests PRIVATE tessera_input tessera_runtime tessera_diagnostics
    tessera_gpu)

add_test(NAME cache_line COMMAND cache_line_tests)

add_executable(node_metadata_tests
    node_metadata_tests.cpp)

target_link_libraries(node_metadata_tests PRIVATE tessera_scene)

add_test(NAME node_metadata COMMAND node_metadata_tests)

# The `NodeScroll`/`NodeMetadata` split (god-type audit section 3.1, steps 2-3): their own contracts
# - the dirty domain each mutation reports, the scroll clamp, and the node forwarders.
add_executable(node_views_tests
    node_views_tests.cpp)

target_link_libraries(node_views_tests PRIVATE tessera_scene)

add_test(NAME node_views COMMAND node_views_tests)

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

# Full-screen state machine and caption hit regions (#232), the platform-neutral half. Pure CPU and
# display-free: the Win32/X11 backends are covered by window_win32/window_x11 and lavapipe smoke.
add_executable(window_fullscreen_tests
    window_fullscreen_tests.cpp)

target_link_libraries(window_fullscreen_tests PRIVATE tessera_hal_window)

add_test(NAME window_fullscreen COMMAND window_fullscreen_tests)

# The normalized OS event model (docs/specs/os-events-v1.md sections 2-3, ADR-023 rules 5 and 7):
# display-capability value rules and fallbacks, event-reason merging, the taxonomy's category/order/
# coalescing/wake rules, the sequential staleness rule, and the total window-path mapping.
# Pure CPU and display-free, so it runs identically on the Windows and Linux gating lanes.
add_executable(os_event_tests
    os_event_tests.cpp)

target_link_libraries(os_event_tests PRIVATE tessera_hal_window)

add_test(NAME os_event COMMAND os_event_tests)

# The X11/Wayland subpixel sources -> PixelGeometry (issue #504). Pure CPU and display-free.
add_executable(subpixel_geometry_tests
    subpixel_geometry_tests.cpp)

target_link_libraries(subpixel_geometry_tests PRIVATE tessera_hal_window)

add_test(NAME subpixel_geometry COMMAND subpixel_geometry_tests)

# The CSS cursor grammar and its diagnostics (issue #191, window-chrome-v1.md section 8.5). Pure CPU
# and display-free: the real native cursor is covered by window_win32/window_x11 and the fake round
# trip by window_cursor_fake.
add_executable(window_cursor_tests
    window_cursor_tests.cpp)

target_link_libraries(window_cursor_tests PRIVATE tessera_style)

add_test(NAME window_cursor COMMAND window_cursor_tests)

# The OS appearance model and the freedesktop settings-portal rules (issue #187, os-events-v1.md
# sections 2 and 5): the normalized color-scheme/reduced-motion snapshot and its documented
# fallbacks with assumption bits, the portal value mapping (including the enable-animations
# inversion), the `Read` reply and `SettingChanged` decoders built with the real message writer, and
# the GTK settings.ini fallback. Pure CPU with no bus, socket, clock or desktop, so it runs
# identically on the Windows and Linux gating lanes; the live portal path is covered by
# `appearance_portal` over a deterministic loopback bus.
add_executable(hal_appearance_tests
    hal_appearance_tests.cpp)

target_link_libraries(hal_appearance_tests PRIVATE tessera_hal_window tessera_dbus)

add_test(NAME hal_appearance COMMAND hal_appearance_tests)

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

    # Xft.rgba -> pixel_geometry end to end (issue #504); skips without an X server.
    add_executable(window_x11_subpixel_tests
        window_x11_subpixel_tests.cpp)

    target_link_libraries(window_x11_subpixel_tests PRIVATE
        tessera_hal_window
        X11::X11)

    add_test(NAME window_x11_subpixel COMMAND window_x11_subpixel_tests)

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

# Wayland window smoke (issue #155): open -> first configure -> redraw/close. Skips with a
# diagnostic on a box without a compositor (WAYLAND_DISPLAY unset or unreachable), so the same ctest
# entry is meaningful on a Wayland desktop, under a headless weston, and in CI where it skips.
if (TESSERA_WAYLAND_FOUND)
    add_executable(window_wayland_tests
        window_wayland_tests.cpp)

    target_link_libraries(window_wayland_tests PRIVATE
        tessera_hal_window)

    add_test(NAME window_wayland COMMAND window_wayland_tests)

    # wl_output subpixel -> pixel_geometry against an in-process fake compositor (issue #504).
    # Needs libwayland-server, which is a separate dev package from the client library; without it
    # the test is not built and the mapper test still covers the enum.
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(TESSERA_WAYLAND_SERVER QUIET wayland-server)
    endif()
    if (TESSERA_WAYLAND_SERVER_FOUND)
        # window_wayland_displays covers the monitor list from the same fake compositor (issue #506).
        foreach (wl_test IN ITEMS window_wayland_subpixel window_wayland_displays)
            add_executable(${wl_test}_tests ${wl_test}_tests.cpp)

            target_include_directories(${wl_test}_tests PRIVATE
                ${TESSERA_WAYLAND_SERVER_INCLUDE_DIRS})
            target_link_directories(${wl_test}_tests PRIVATE
                ${TESSERA_WAYLAND_SERVER_LIBRARY_DIRS})
            target_link_libraries(${wl_test}_tests PRIVATE
                tessera_hal_window
                ${TESSERA_WAYLAND_SERVER_LIBRARIES})

            add_test(NAME ${wl_test} COMMAND ${wl_test}_tests)
        endforeach()
    else()
        message(STATUS "libwayland-server not found: Wayland fake-compositor tests disabled")
    endif()
    set_tests_properties(window_wayland PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")
endif()

# The pure X11 keysym/button/modifier mapping has no Xlib dependency, so it runs on every platform.
add_executable(x11_input_translation_tests
    x11_input_translation_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_input_translation.cpp)

target_link_libraries(x11_input_translation_tests PRIVATE tessera_input)

target_include_directories(x11_input_translation_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/linux)

add_test(NAME x11_input_translation COMMAND x11_input_translation_tests)

# The pure XInput2 rules (scroll valuators, touch slots, pen normalization) and the XIM pre-edit
# composition state machine have no Xlib dependency either, so they run on every platform too.
add_executable(x11_input_translation_xi2_tests
    x11_input_translation_xi2_tests.cpp
    x11_input_translation_xi2_ime_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_input_translation.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_xi2_translation.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/linux/x11_ime_composition.cpp)

target_link_libraries(x11_input_translation_xi2_tests PRIVATE tessera_input)

target_include_directories(x11_input_translation_xi2_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/linux)

add_test(NAME x11_xi2_translation COMMAND x11_input_translation_xi2_tests)

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

# Android presentation family (VK_KHR_android_surface): the enumerator is additive, distinct from
# every desktop family, and `DeviceDesc` still defaults to Auto. Links only the RHI seam, so it runs
# on every lane including a null-backend build with no Vulkan SDK.
add_executable(android_surface_platform_tests
    android_surface_platform_tests.cpp)

target_link_libraries(android_surface_platform_tests PRIVATE tessera_rhi)

add_test(NAME android_surface_platform COMMAND android_surface_platform_tests)

# Android window backend (issue #189): the density arithmetic and the family enumerator. Links only
# the HAL window seam, so it runs on every lane; the extent/scale setters need a real ANativeWindow
# and are covered by the emulator lane.
add_executable(android_window_tests
    hal/android_window_tests.cpp)

target_link_libraries(android_window_tests PRIVATE tessera_hal_window)

add_test(NAME android_window COMMAND android_window_tests)

# Android input mapping rules (issue #189): pure over stable ABI constants, so it runs on every lane
# without an NDK or a device. The source lives under src/platform/android but includes no NDK header.
add_executable(android_input_tests
    hal/android_input_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/android/android_input_translation.cpp)

target_include_directories(android_input_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/android)

target_link_libraries(android_input_tests PRIVATE tessera_input)

add_test(NAME android_input COMMAND android_input_tests)

# Android point-to-caret translation (issue #189, android-lane.md section 3.13): pure over plain
# numbers, so it runs on every lane without an NDK or a device. The box arithmetic - physical pixels
# to the content box, the padding, the density scale - is where a caret lands one character off.
add_executable(android_caret_tests
    hal/android_caret_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/android/android_caret_translation.cpp)

target_include_directories(android_caret_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/android)

target_link_libraries(android_caret_tests PRIVATE tessera_text)

add_test(NAME android_caret COMMAND android_caret_tests)

# Android long-press and drag-to-select (issue #189, android-lane.md section 3.14): pure over plain
# numbers, so it runs on every lane without an NDK or a device. The thresholds (500 ms, 8 dp) and the
# transitions are the whole behaviour, and a wrong one turns every scroll into a selection.
add_executable(android_selection_tests
    hal/android_selection_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/android/android_selection_gesture.cpp)

target_include_directories(android_selection_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/android)

add_test(NAME android_selection COMMAND android_selection_tests)

# Android clipboard text conversion (issue #189, android-lane.md section 3.16): pure over plain
# bytes, so it runs on every lane without an NDK or a device. The rule that bites is that a code
# point above U+FFFF is a SURROGATE PAIR, two UTF-16 units; one unit per code point turns every
# emoji into a mojibake box and makes every offset after it wrong.
add_executable(android_clipboard_text_tests
    hal/android_clipboard_text_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/android/android_clipboard_text.cpp)

target_include_directories(android_clipboard_text_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/android)

add_test(NAME android_clipboard_text COMMAND android_clipboard_text_tests)

# Android window-metrics decode (issue #189): pure over plain numbers, so it runs on every lane
# without an NDK or a device. The source lives under src/platform/android but includes no Android
# header.
add_executable(android_metrics_tests
    hal/android_metrics_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/android/android_metrics_translation.cpp)

target_include_directories(android_metrics_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/android)

target_link_libraries(android_metrics_tests PRIVATE tessera_hal_metrics)

add_test(NAME android_metrics COMMAND android_metrics_tests)

# Android accessibility mapping (issue #189): the role-to-class-name table and the action bits.
# Pure over the engine's own snapshot type, so it runs on every lane with no Android SDK and no
# device; the enabled inversion and the text-field value/label split are the reason it exists.
add_executable(android_a11y_tests
    hal/android_a11y_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/android/android_a11y_mapping.cpp)

target_include_directories(android_a11y_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/android)

target_link_libraries(android_a11y_tests PRIVATE tessera_accessibility)

add_test(NAME android_a11y COMMAND android_a11y_tests)

# Java-MotionEvent translation (issue #189): the shell is a Java activity, so touch arrives as a
# MotionEvent rather than an AInputEvent. The mapping is pure over plain numbers, so it runs on
# every lane with no NDK, no JNI and no device.
add_executable(android_motion_tests
    hal/android_motion_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/android/android_motion_translation.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/android/android_input_translation.cpp)

target_include_directories(android_motion_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/android)

target_link_libraries(android_motion_tests PRIVATE tessera_input)

add_test(NAME android_motion COMMAND android_motion_tests)

# Android IME translation (issue #189): the InputConnection callbacks mapped onto ui::InputEvent.
# Pure over UTF-16 code units and offsets, so it runs on every lane with no Android SDK and no
# device; the surrogate-pair and caret-offset conversions are the reason it exists.
add_executable(android_ime_tests
    hal/android_ime_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/android/android_ime_translation.cpp)

target_include_directories(android_ime_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/android)

target_link_libraries(android_ime_tests PRIVATE tessera_input)

add_test(NAME android_ime COMMAND android_ime_tests)
# Fold decoding (issue #189): a flat fold is a seam, not a barrier, and that rule is the one most
# likely to be got wrong, so it gets its own executable and its own failure line.
add_executable(android_fold_tests
    hal/android_fold_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/platform/android/android_metrics_translation.cpp)

target_include_directories(android_fold_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/platform/android)

target_link_libraries(android_fold_tests PRIVATE tessera_hal_metrics)

add_test(NAME android_fold COMMAND android_fold_tests)

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

# The baseline is resident bytes per node measured on an uninstrumented Release build. A sanitizer
# build adds its own shadow memory and redzones to every allocation, so the number is a different
# claim, not a regression; the sanitizer presets exclude the label (CMakePresets.json).
set_tests_properties(memory_steady_state PROPERTIES LABELS uninstrumented_baseline)

add_executable(diagnostics_trace_tests
    diagnostics_trace_tests.cpp)

target_link_libraries(diagnostics_trace_tests PRIVATE tessera_diagnostics)

add_test(NAME diagnostics_trace COMMAND diagnostics_trace_tests)

# Byte-level guarantee that the production profile ships no tracing code: the test reads the
# tessera_diagnostics archive and fails if the recorder's own markers are in it while
# TESSERA_ENABLE_TRACING=OFF (and proves it can see them when the option is ON). This is what turns
# "the linker happened to drop trace.obj" into a contract (ADR-022 decision 11).
add_executable(tracing_strip_tests
    tracing_strip_tests.cpp)

target_link_libraries(tracing_strip_tests PRIVATE tessera_diagnostics)

add_test(NAME tracing_strip COMMAND tracing_strip_tests $<TARGET_FILE:tessera_diagnostics>)

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

add_executable(data_store_tests
    data_store_tests.cpp)

target_link_libraries(data_store_tests PRIVATE tessera_runtime)

add_test(NAME data_store COMMAND data_store_tests)

add_executable(worker_executor_tests
    worker_executor_tests.cpp)

target_link_libraries(worker_executor_tests PRIVATE tessera_runtime)

add_test(NAME worker_executor COMMAND worker_executor_tests)

add_executable(task_tests
    task_tests.cpp)

target_link_libraries(task_tests PRIVATE tessera_runtime)

add_test(NAME task COMMAND task_tests)

# Issues #843, #845 and #842: an undroppable resume, cancellation reasons/linked sources/deadlines,
# and the one-wake-per-edge hook. Split from `task_tests` so neither file passes the 500-line limit.
add_executable(task_cancellation_tests
    task_cancellation_tests.cpp)

target_link_libraries(task_cancellation_tests PRIVATE tessera_runtime)

add_test(NAME task_cancellation COMMAND task_cancellation_tests)

# Issue #844: the task combinators (when_all/when_any/when_any_value) and the time helpers
# (delay/timeout), including the eager-cancellation wakeup they lean on. Split from `task_tests`
# so neither file passes the 500-line limit.
add_executable(task_combinator_tests
    task_combinator_tests.cpp)

target_link_libraries(task_combinator_tests PRIVATE tessera_runtime)

add_test(NAME task_combinators COMMAND task_combinator_tests)
# The time-based combinator tests (issue #844): delay, timeout and the eager-cancellation
# wakeup, split from task_combinator_tests to keep both under the source-size limit.
add_executable(task_time_tests
    task_time_tests.cpp)

target_link_libraries(task_time_tests PRIVATE tessera_runtime)

add_test(NAME task_time COMMAND task_time_tests)

add_executable(frame_pacer_tests
    frame_pacer_tests.cpp)

target_link_libraries(frame_pacer_tests PRIVATE tessera_runtime)

add_test(NAME frame_pacer COMMAND frame_pacer_tests)

# The adaptive present cap (docs/design/performance.md section 6, step 3; runtime-config-v1.md 5.2
# and 5.7): the pure refresh-divisor rule - exact divisors, the documented floor, the maxFps
# ceiling, an unknown refresh (no divisor) and step 0 as identity - the controller's documented cap
# sequence and its disabled paths, and the proof that the decided cap reaches the pacer through its
# existing scripted-rate-change path. Pure CPU: no GPU, window or wall clock.
add_executable(present_cap_tests
    present_cap_tests.cpp)

target_include_directories(present_cap_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/runtime)

target_link_libraries(present_cap_tests PRIVATE tessera_runtime)

add_test(NAME present_cap COMMAND present_cap_tests)

# The display-refresh source behind that cap (docs/specs/runtime-config-v1.md 5.2 and 6.2): the
# pure validation/mapping table (0, negative, NaN, huge, 60, 144, 59.94), "unknown stays unknown",
# "an explicit option wins", the X11 RandR mode math, and the hand-off through
# window_adaptive_quality_policy to the step-3 divisor. The platform reads themselves (Win32
# DEVMODE, X11 RandR) are exercised by the window tests; no display is needed here.
add_executable(refresh_rate_tests
    refresh_rate_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/hal/window/refresh_rate.cpp)

target_include_directories(refresh_rate_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app
    ${PROJECT_SOURCE_DIR}/src/runtime)

target_link_libraries(refresh_rate_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME refresh_rate COMMAND refresh_rate_tests)

# The `runtime` object of tessera.json (docs/specs/runtime-config-v1.md): the model defaults, the
# field table's clamp-vs-error split, enum strings, typed errors and unknown-key warnings.
add_executable(runtime_config_tests
    runtime_config_tests.cpp)

target_link_libraries(runtime_config_tests PRIVATE tessera_runtime)

add_test(NAME runtime_config COMMAND runtime_config_tests)

# The one JSON failure vocabulary (god-type audit section 4.4): every code has a distinct non-empty
# description with no generic fallthrough, and the harness's public alias is that same type rather
# than a private copy. The schema reader is excluded on purpose -- it is a leaf library that may
# include no project header, enforced by schema_layering.
add_executable(json_error_vocabulary_tests
    json_error_vocabulary_tests.cpp)

target_link_libraries(json_error_vocabulary_tests PRIVATE
    tessera_harness)

add_test(NAME json_error_vocabulary COMMAND json_error_vocabulary_tests)

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

# Replaces the global operator new/delete to count allocations; see the helper's comment.
tessera_allow_allocator_override(selector_match_tests)

add_test(NAME selector_match COMMAND selector_match_tests)

# `:has()` and `@scope` (issue #622): the selector half (parse, match, specificity, `@supports`) and
# the at-rule half (a rule applies only inside the region), plus their composition with `@layer`.
add_executable(theme_scoping_tests
    theme_scoping_tests.cpp)

target_link_libraries(theme_scoping_tests PRIVATE tessera_style)

add_test(NAME theme_scoping COMMAND theme_scoping_tests)

add_executable(cascade_tests
    cascade_tests.cpp)

target_link_libraries(cascade_tests PRIVATE tessera_style)

add_test(NAME cascade COMMAND cascade_tests)

add_executable(style_var_tests
    style_var_tests.cpp)

target_link_libraries(style_var_tests PRIVATE tessera_style)

add_test(NAME style_var COMMAND style_var_tests)

# The computed style's representation (issue #735, slices E and F): a delta over the registry's
# shared initial values, with a stable small id per registered longhand.
add_executable(style_delta_tests
    style_delta_tests.cpp)

target_link_libraries(style_delta_tests PRIVATE tessera_style)

add_test(NAME style_delta COMMAND style_delta_tests)

add_executable(compiled_stylesheet_tests
    compiled_stylesheet_tests.cpp)

target_link_libraries(compiled_stylesheet_tests PRIVATE tessera_style)

add_test(NAME compiled_stylesheet COMMAND compiled_stylesheet_tests)

add_executable(ua_stylesheet_tests
    ua_stylesheet_tests.cpp)
target_include_directories(ua_stylesheet_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(ua_stylesheet_tests PRIVATE tessera_style)
add_test(NAME ua_stylesheet COMMAND ua_stylesheet_tests)

add_executable(scene_style_attributes_tests
    scene_style_attributes_tests.cpp)
target_include_directories(scene_style_attributes_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(scene_style_attributes_tests PRIVATE tessera_style tessera_scene)
add_test(NAME scene_style_attributes COMMAND scene_style_attributes_tests)

add_executable(stylesheet_blob_tests
    stylesheet_blob_tests.cpp)

target_link_libraries(stylesheet_blob_tests PRIVATE tessera_style)

add_test(NAME stylesheet_blob COMMAND stylesheet_blob_tests)

# Theme asset model (issue #81, ADR-048; stylesheet-v1.md section 9.1): the build-time parse with
# the spec's typed errors, the token-missing warning, and the deterministic blob round-trip.
add_executable(theme_asset_tests
    theme_asset_tests.cpp)

target_link_libraries(theme_asset_tests PRIVATE tessera_style)

add_test(NAME theme_asset COMMAND theme_asset_tests)

# Theme versions and pins (issue #237, ADR-047 item 4): themeVersions parse, the SemVer pin rule,
# TESSERA_THEME_VERSION_PIN, and the version surviving the blob round trip.
add_executable(theme_version_tests
    theme_version_tests.cpp)

target_link_libraries(theme_version_tests PRIVATE tessera_style)

add_test(NAME theme_version COMMAND theme_version_tests)

# The Instrument theme asset (issue #237, ADR-047 item 5): clean parse, identical token sets,
# version 1.0.0, and WCAG 2.2 AA contrast for every text and indicator pair of every theme.
add_executable(instrument_theme_tests
    instrument_theme_tests.cpp)

target_link_libraries(instrument_theme_tests PRIVATE tessera_style)

target_compile_definitions(instrument_theme_tests PRIVATE
    TESSERA_INSTRUMENT_THEME="${CMAKE_SOURCE_DIR}/packages/tessera-ui/themes/instrument.theme.json")

add_test(NAME instrument_theme COMMAND instrument_theme_tests)

add_executable(property_registry_tests
    property_registry_tests.cpp)

target_link_libraries(property_registry_tests PRIVATE tessera_style)

add_test(NAME property_registry COMMAND property_registry_tests)

# Every registered property has a real effect (issue #213): `order` reorders flex items in a layout
# result, `visibility` leaves paint/hit testing/a11y while keeping its box, `text-overflow` reaches
# painted output, `container-type`/`container-name` select the `@container` ancestor, and
# `font-style` reaches the text seam. Each test goes through the real path, never "the computed
# value exists".
add_executable(property_effects_tests
    property_effects_tests.cpp)

target_link_libraries(property_effects_tests PRIVATE
    tessera_accessibility
    tessera_layout
    tessera_paint
    tessera_style
    tessera_text)

add_test(NAME property_effects COMMAND property_effects_tests)

# The per-property status table in docs/specs/stylesheet-property-status-v1.md covers exactly the
# registry's longhands, and every `applied` row names a test (issue #213). The doc path is a compile
# definition so the cross-check reads the real file.
add_executable(property_status_tests
    property_status_tests.cpp)

target_link_libraries(property_status_tests PRIVATE tessera_style)

target_compile_definitions(property_status_tests PRIVATE
    TESSERA_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs")

add_test(NAME property_status COMMAND property_status_tests)

add_executable(logical_properties_tests
    logical_properties_tests.cpp)

target_link_libraries(logical_properties_tests PRIVATE tessera_paint tessera_style)

add_test(NAME logical_properties COMMAND logical_properties_tests)

add_executable(media_query_tests
    media_query_tests.cpp)

target_link_libraries(media_query_tests PRIVATE tessera_style)

add_test(NAME media_query COMMAND media_query_tests)

add_executable(viewport_units_tests
    viewport_units_tests.cpp)

target_link_libraries(viewport_units_tests PRIVATE tessera_style)

add_test(NAME viewport_units COMMAND viewport_units_tests)

add_executable(spec_units_tests
    spec_units_tests.cpp)

target_link_libraries(spec_units_tests PRIVATE tessera_style)

add_test(NAME spec_units COMMAND spec_units_tests)

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

# `light-dark()` and `color-mix()` (issues #620, #621): the two colour functions that take other
# colours as arguments, plus the computed-value-time substitution that resolves them inside a larger
# value and the `color-scheme` property they read.
add_executable(color_functions_tests
    color_functions_tests.cpp)

target_link_libraries(color_functions_tests PRIVATE tessera_style)

add_test(NAME color_functions COMMAND color_functions_tests)

add_executable(animation_tests
    animation_tests.cpp)

target_link_libraries(animation_tests PRIVATE tessera_animation)

add_test(NAME animation COMMAND animation_tests)

add_executable(box_shadow_interpolation_tests
    box_shadow_interpolation_tests.cpp)

target_link_libraries(box_shadow_interpolation_tests PRIVATE tessera_animation)

add_test(NAME box_shadow_interpolation COMMAND box_shadow_interpolation_tests)

# `text-shadow` interpolation (issue #616): the same layer walk without spread/inset, and the
# emitted value stays parseable by the paint pass.
add_executable(text_shadow_interpolation_tests
    text_shadow_interpolation_tests.cpp)

target_link_libraries(text_shadow_interpolation_tests PRIVATE tessera_animation)

add_test(NAME text_shadow_interpolation COMMAND text_shadow_interpolation_tests)

# `background-image` gradient interpolation (issue #613): linear and radial stop/geometry blending,
# the discrete shape and repeating mismatches, and the endpoints.
add_executable(gradient_interpolation_tests
    gradient_interpolation_tests.cpp)

target_link_libraries(gradient_interpolation_tests PRIVATE tessera_animation)

add_test(NAME gradient_interpolation COMMAND gradient_interpolation_tests)

add_executable(animation_quality_pause_tests
    animation_quality_pause_tests.cpp)

target_link_libraries(animation_quality_pause_tests PRIVATE tessera_animation)

add_test(NAME animation_quality_pause COMMAND animation_quality_pause_tests)

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

add_executable(logical_layout_tests
    logical_layout_tests.cpp)

target_link_libraries(logical_layout_tests PRIVATE tessera_layout tessera_style)

add_test(NAME logical_layout COMMAND logical_layout_tests)

add_executable(layout_cache_tests
    layout_cache_tests.cpp)

target_link_libraries(layout_cache_tests PRIVATE tessera_layout)

add_test(NAME layout_cache COMMAND layout_cache_tests)

# Layout's text-fit fact (issue #490): a node's plain-text payload provably stays inside the content
# box paint uses (the border box minus the `padding-*` longhands, with paint's own wrap decision), so
# the paint-pass cull can stop refusing every text node. Fits / Overflows / Unknown, the padding-only
# content box, and `apply_layout` copying the fact onto the node. Pure CPU, stub text metrics.
add_executable(layout_text_fit_tests
    layout_text_fit_tests.cpp)

target_link_libraries(layout_text_fit_tests PRIVATE
    tessera_layout
    tessera_text)

add_test(NAME layout_text_fit COMMAND layout_text_fit_tests)

add_executable(text_measure_tests
    text_measure_tests.cpp)

target_link_libraries(text_measure_tests PRIVATE tessera_text)

add_test(NAME text_measure COMMAND text_measure_tests)

add_executable(text_measure_cache_tests
    text_measure_cache_tests.cpp)

target_link_libraries(text_measure_cache_tests PRIVATE tessera_text)

add_test(NAME text_measure_cache COMMAND text_measure_cache_tests)

# Long-run cache bound (issue #706, memory-model-v1 section 10): a scrolling list plus a per-frame
# restyle for 600 frames, asserting the measure cache stays within its capacity and does not keep
# climbing. A bound, not a benchmark, so it runs on every lane.
add_executable(cache_growth_tests
    cache_growth_tests.cpp)

target_link_libraries(cache_growth_tests PRIVATE tessera_text)

add_test(NAME cache_growth COMMAND cache_growth_tests)

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

# Issue #314: a content-sized text box must agree with the glyph ink it contains. Installs the
# real-face measurer the app window and frame capture use, so layout and paint share one face.
add_executable(text_measure_paint_tests
    text_measure_paint_tests.cpp)

target_link_libraries(text_measure_paint_tests PRIVATE tessera_layout tessera_text)

add_test(NAME text_measure_paint COMMAND text_measure_paint_tests "${TESSERA_FONT_FIXTURE}")

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

add_executable(background_image_tests
    background_image_tests.cpp)

target_link_libraries(background_image_tests PRIVATE tessera_paint tessera_text tessera_style)

add_test(NAME background_image COMMAND background_image_tests)

# Several background layers (issue #614): the `background-image` layer list, the `background`
# shorthand that produces it, the CSS bottom-to-top paint order, and the layer-wise interpolation.
add_executable(background_layers_tests
    background_layers_tests.cpp)

target_link_libraries(background_layers_tests PRIVATE tessera_paint tessera_text tessera_style)

add_test(NAME background_layers COMMAND background_layers_tests)

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

# A percentage width resolves against the containing block's content box (issue #791 part 2). The
# Android shell's card has `padding: 24px` and its children ask for `width: 100%`; resolving against
# the padding box makes every child end at the card's border edge, so the right padding is never
# subtracted and the children overflow. This goes through the real path - a compiled stylesheet plus
# inline declarations - because the pure LayoutInput path takes longhands that are already expanded.
add_executable(layout_percent_content_box_tests
    layout_percent_content_box_tests.cpp)

target_link_libraries(layout_percent_content_box_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME layout_percent_content_box COMMAND layout_percent_content_box_tests)

add_executable(layout_observer_tests
    layout_observer_tests.cpp)

target_link_libraries(layout_observer_tests PRIVATE tessera_layout)

# Replaces the global operator new/delete to count allocations; see the helper's comment.
tessera_allow_allocator_override(layout_observer_tests)

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

add_executable(frame_pipeline_interpolation_suppress_tests
    frame_pipeline_interpolation_suppress_tests.cpp)

target_link_libraries(frame_pipeline_interpolation_suppress_tests PRIVATE tessera_frame tessera_runtime)

add_test(NAME frame_pipeline_interpolation_suppress COMMAND frame_pipeline_interpolation_suppress_tests)

add_executable(frame_pipeline_style_sync_tests
    frame_pipeline_style_sync_tests.cpp)

target_link_libraries(frame_pipeline_style_sync_tests PRIVATE tessera_frame tessera_runtime)

add_test(NAME frame_pipeline_style_sync COMMAND frame_pipeline_style_sync_tests)

add_executable(frame_pipeline_shadow_damage_tests
    frame_pipeline_shadow_damage_tests.cpp)

target_link_libraries(frame_pipeline_shadow_damage_tests PRIVATE tessera_frame tessera_runtime)

add_test(NAME frame_pipeline_shadow_damage COMMAND frame_pipeline_shadow_damage_tests)

# Transition and animation declarations reach the frame pipeline (issue #271): a stylesheet
# `transition: opacity 200ms linear` starts an AnimationId from `FramePipeline::sync_style` and the
# sampled overlay moves the computed value, and the same call drives the `animation-*` bridge.
add_executable(transition_wiring_tests
    transition_wiring_tests.cpp)

target_link_libraries(transition_wiring_tests PRIVATE tessera_frame tessera_runtime)

add_test(NAME transition_wiring COMMAND transition_wiring_tests)

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

# ADR-051 slice 3: an object literal in a declared record's position becomes an `ir::Record`
# (docs/specs/ir-v1-expr.md section 4.3). The implementation landed with PR #339 but had no test of
# its own -- the IR-level tests prove the format and `lowering_props` proves the prop table, so
# nothing proved that a TSX object literal actually reaches the IR as a record value.
add_executable(lowering_record_value_tests
    lowering_record_value_tests.cpp)

target_link_libraries(lowering_record_value_tests PRIVATE tessera_lowering)

add_test(NAME lowering_record_value COMMAND lowering_record_value_tests)
# Issue #804: an anonymous record type in a props value position (`{ rows: { id: string }[] }`)
# becomes a `types` entry under a synthesized name, and a `.filter(...)`/`.slice(...)` chain lowers
# as a list loop source.
add_executable(lowering_anonymous_record_tests
    lowering_anonymous_record_tests.cpp)

target_link_libraries(lowering_anonymous_record_tests PRIVATE tessera_lowering)

add_test(NAME lowering_anonymous_record COMMAND lowering_anonymous_record_tests)

# Issue #70 cross-file props aliases: the same alias imported from another file must lower to the
# same records (and the same generated C++) as the inline alias.
add_executable(lowering_props_alias_tests
    lowering_props_alias_tests.cpp)

target_link_libraries(lowering_props_alias_tests PRIVATE tessera_lowering tessera_codegen tessera_ir_ids)

add_test(NAME lowering_props_alias COMMAND lowering_props_alias_tests)

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

# `environment` hook lowering (docs/specs/jsx-subset-v1.md section 7.1, issue #692): a TSX
# `useBreakpoint()`/`useTheme()` call becomes an IR `environment[]` entry with the provider, the type
# and whether it is writable.
add_executable(lowering_environment_tests
    lowering_environment_tests.cpp)

target_link_libraries(lowering_environment_tests PRIVATE tessera_lowering)

add_test(NAME lowering_environment COMMAND lowering_environment_tests)

# Spike #659 (ADR-065): the custom-hook expansion pass, off by default. The differential test needs
# the whole front end plus lowering, because it compares two components' IR.
add_executable(hook_expansion_differential_tests
    hook_expansion_differential_tests.cpp)

target_link_libraries(hook_expansion_differential_tests PRIVATE tessera_lowering)

add_test(NAME hook_expansion_differential COMMAND hook_expansion_differential_tests)

add_executable(hook_expansion_tests
    hook_expansion_tests.cpp)

target_link_libraries(hook_expansion_tests PRIVATE tessera_lowering)

add_test(NAME hook_expansion COMMAND hook_expansion_tests)

add_executable(lowering_behaviors_tests
    lowering_behaviors_tests.cpp)

target_link_libraries(lowering_behaviors_tests PRIVATE tessera_lowering)

add_test(NAME lowering_behaviors COMMAND lowering_behaviors_tests)

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

# Slice 8 virtual collections, split from `lowering_control_tests` when that file reached the
# 500-line limit.
add_executable(lowering_virtual_control_tests
    lowering_virtual_control_tests.cpp)

target_link_libraries(lowering_virtual_control_tests PRIVATE tessera_lowering)

add_test(NAME lowering_virtual_control COMMAND lowering_virtual_control_tests)

# ADR-066 item 2: control flow in a handler body (`if`/`else`, `const`, an early `return`).
add_executable(lowering_handler_control_tests
    lowering_handler_control_tests.cpp)

target_link_libraries(lowering_handler_control_tests PRIVATE tessera_lowering)

add_test(NAME lowering_handler_control COMMAND lowering_handler_control_tests)

# ADR-066 item 4: the `useReducer` lowering: one state cell, one handler per case, and a
# `dispatch({ type: "x" })` that becomes a `Dispatch` action.
add_executable(lowering_reducer_tests
    lowering_reducer_tests.cpp)

target_link_libraries(lowering_reducer_tests PRIVATE tessera_lowering)

add_test(NAME lowering_reducer COMMAND lowering_reducer_tests)

# ADR-066 item 2, the generated C++ half: an `If` action becomes a real `if`/`else`.
add_executable(codegen_handler_control_tests
    codegen_handler_control_tests.cpp)

target_link_libraries(codegen_handler_control_tests PRIVATE tessera_codegen)

add_test(NAME codegen_handler_control COMMAND codegen_handler_control_tests)

# ADR-066 item 4, the generated C++ half: a `Dispatch` action becomes a call of the handler its
# case became, resolved through the state's dispatch table at generation time.
add_executable(codegen_reducer_tests
    codegen_reducer_tests.cpp)

target_link_libraries(codegen_reducer_tests PRIVATE tessera_codegen)

add_test(NAME codegen_reducer COMMAND codegen_reducer_tests)

# ADR-066 item 3: `filter`, `slice`, `.length` and the total string methods.
add_executable(lowering_list_ops_tests
    lowering_list_ops_tests.cpp)

target_link_libraries(lowering_list_ops_tests PRIVATE tessera_lowering)

add_test(NAME lowering_list_ops COMMAND lowering_list_ops_tests)
# Issue #803: the strict whole-string parse behind parseIntOr/parseFloatOr. These are the C++
# pins; `ir_eval` pins the same rule for the evaluator, so the two cannot drift.
add_executable(parse_number_or_tests
    parse_number_or_tests.cpp)

target_link_libraries(parse_number_or_tests PRIVATE tessera_scene)

add_test(NAME parse_number_or COMMAND parse_number_or_tests)
# Issue #802: the static context form. The rules/lowering half; `codegen_context_runtime` covers
# the generated C++ (the provider push and the default fallback).
add_executable(lowering_context_tests
    lowering_context_tests.cpp)

target_link_libraries(lowering_context_tests PRIVATE tessera_lowering)

add_test(NAME lowering_context COMMAND lowering_context_tests)

add_executable(lowering_control_reject_tests
    lowering_control_reject_tests.cpp)

target_link_libraries(lowering_control_reject_tests PRIVATE tessera_lowering)

add_test(NAME lowering_control_reject COMMAND lowering_control_reject_tests)

add_executable(lowering_local_inline_tests
    lowering_local_inline_tests.cpp)

target_link_libraries(lowering_local_inline_tests PRIVATE tessera_lowering)

add_test(NAME lowering_local_inline COMMAND lowering_local_inline_tests)

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

target_link_libraries(lowering_module_stylesheet_tests PRIVATE tessera_lowering tessera_codegen tessera_ir_ids)

add_test(NAME lowering_module_stylesheet COMMAND lowering_module_stylesheet_tests)

add_executable(lowering_module_determinism_tests
    lowering_module_determinism_tests.cpp)

target_link_libraries(lowering_module_determinism_tests PRIVATE
    tessera_lowering
    Threads::Threads)

add_test(NAME lowering_module_determinism COMMAND lowering_module_determinism_tests)

# Field/Form are authored as real TSX sources (issue #70): the build lowers the two files under
# packages/tessera-ui/ into library_components_generated.cpp, and this test re-lowers the committed
# sources in-process to prove the embedded declaration is not stale and cannot drift.
add_executable(library_tsx_parity_tests
    library_tsx_parity_tests.cpp)

target_link_libraries(library_tsx_parity_tests PRIVATE tessera_lowering)

target_compile_definitions(library_tsx_parity_tests PRIVATE
    TESSERA_LIBRARY_TSX_DIR="${CMAKE_SOURCE_DIR}/packages/tessera-ui")

add_test(NAME library_tsx_parity COMMAND library_tsx_parity_tests)

add_executable(paint_pass_tests
    paint_pass_tests.cpp)

target_link_libraries(paint_pass_tests PRIVATE
    tessera_paint
    tessera_rhi_null)

add_test(NAME paint_pass COMMAND paint_pass_tests)

# Retained draw list per clean subtree (issue #476 slice 1): a clean subtree replays as one flat list
# byte-identically, a change under it or another cull rect never replays a stale one, and budget 0
# turns it off. Pure CPU.
add_executable(subtree_paint_cache_tests
    subtree_paint_cache_tests.cpp)

target_link_libraries(subtree_paint_cache_tests PRIVATE
    tessera_paint)

add_test(NAME subtree_paint_cache COMMAND subtree_paint_cache_tests)

# Paint-pass culling (issue #487): a node whose painted reach misses PaintContext::cull skips its
# style read and emission, a clip-bounded subtree is skipped whole, and the pixels inside the rect
# (deterministic CPU canvas) and the emitted commands are identical to an unculled paint. Text that
# leaves its own box, transform frames and non-clipping parents are never culled. Pure CPU.
add_executable(paint_cull_tests
    paint_cull_tests.cpp)

target_link_libraries(paint_cull_tests PRIVATE
    tessera_paint
    tessera_software)

add_test(NAME paint_cull COMMAND paint_cull_tests)

# The paint side of layout's text-fit hint (issue #490): `Node::text_fits_box` lets the paint-pass cull
# bound a plain-text payload by its box, so a fitting text node outside the rect is skipped and counted.
# Every payload paint adds on top of the plain text layout measured (spans, decoration, `max_lines`, a
# font override) and any rect that is not the layout box stays unbounded. Pure CPU, stub metrics.
add_executable(paint_cull_text_fit_tests
    paint_cull_text_fit_tests.cpp)

target_link_libraries(paint_cull_text_fit_tests PRIVATE
    tessera_frame
    tessera_paint
    tessera_software)

add_test(NAME paint_cull_text_fit COMMAND paint_cull_text_fit_tests)

# Subtree cull (issue #735, slice C): `Node::subtree_paint_bounds` caches the union of a subtree's
# painted boxes so the paint pass rejects a whole subtree at its root in O(1) instead of visiting
# every node in it. The cache is invalidated by every mutation that moves a painted pixel (through
# `mark_dirty`, plus the two writes that mark no dirty domain), an unbounded subtree is never culled,
# and a culled pass emits exactly the ops the unculled pass emitted inside the rect. Pure CPU.
add_executable(paint_subtree_cull_tests
    paint_subtree_cull_tests.cpp)

target_link_libraries(paint_subtree_cull_tests PRIVATE
    tessera_paint)

add_test(NAME paint_subtree_cull COMMAND paint_subtree_cull_tests)

# Effective opacity (#227): the ancestor product multiplies each emitted primitive (box visuals and
# text/vector/image content), an animated ancestor fade repaints the subtree, and the paint cache
# never replays a record under a different inherited product.
add_executable(paint_opacity_tests
    paint_opacity_tests.cpp)

target_link_libraries(paint_opacity_tests PRIVATE
    tessera_frame
    tessera_runtime)

add_test(NAME paint_opacity COMMAND paint_opacity_tests)

# Per-run LCD eligibility at the paint seam (#471): the window permission, the per-node narrowing
# (opacity 1, translate-only) and the paint cache's record/replay of the mark.
add_executable(paint_lcd_eligibility_tests
    paint_lcd_eligibility_tests.cpp)

target_link_libraries(paint_lcd_eligibility_tests PRIVATE tessera_paint)

add_test(NAME paint_lcd_eligibility COMMAND paint_lcd_eligibility_tests)

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

# `clip-path` basic shapes (ADR-068 slice 4, issue #672): the parser, the record it produces, and
# the coverage rule the CPU reference and the shader share.
add_executable(clip_path_tests
    clip_path_tests.cpp)

target_link_libraries(clip_path_tests PRIVATE tessera_paint tessera_software)

add_test(NAME clip_path COMMAND clip_path_tests)

add_executable(paint_text_tests
    paint_text_tests.cpp)

target_link_libraries(paint_text_tests PRIVATE tessera_paint tessera_frame tessera_runtime)

add_test(NAME paint_text COMMAND paint_text_tests)

# `text-shadow` (issue #616): grammar, paint order behind every glyph of the block (spans
# included), the bounded blur stack, the opacity fold, and the layout/ink-box invariants.
add_executable(paint_text_shadow_tests
    paint_text_shadow_tests.cpp)

target_link_libraries(paint_text_shadow_tests PRIVATE tessera_paint tessera_scene)

add_test(NAME paint_text_shadow COMMAND paint_text_shadow_tests)

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

# The caret box paint draws, exposed so a platform IME can answer `requestCursorUpdates` with the
# SAME geometry (issue #189, android-lane.md section 3.18). Pure over a laid-out `TextLayout`, so it
# runs on every lane with no GPU and no device.
add_executable(paint_caret_box_tests
    paint_caret_box_tests.cpp)

target_link_libraries(paint_caret_box_tests PRIVATE
    tessera_paint)

add_test(NAME paint_caret_box COMMAND paint_caret_box_tests)

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

# The project-file policy the MCP server and the Wasm module share (designer-studio-v1.md section 7).
add_executable(schema_project_files_tests
    schema_project_files_tests.cpp)

target_link_libraries(schema_project_files_tests PRIVATE tessera_schema)

add_test(NAME schema_project_files COMMAND schema_project_files_tests)

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

add_executable(layout_block_fill_tests
    layout_block_fill_tests.cpp)

target_link_libraries(layout_block_fill_tests PRIVATE tessera_layout)

add_test(NAME layout_block_fill COMMAND layout_block_fill_tests)

add_executable(paint_wrap_policy_tests
    paint_wrap_policy_tests.cpp)

target_link_libraries(paint_wrap_policy_tests PRIVATE tessera_scene)

add_test(NAME paint_wrap_policy COMMAND paint_wrap_policy_tests)

add_executable(swapchain_tests
    swapchain_tests.cpp)

target_link_libraries(swapchain_tests PRIVATE tessera_rhi_null)

add_test(NAME swapchain COMMAND swapchain_tests)

# The shared retire rule (ADR-053 decision 2): backend-independent, so it runs on every platform
# with a fake completion source and needs no GPU.
add_executable(retire_queue_tests
    retire_queue_tests.cpp)

target_link_libraries(retire_queue_tests PRIVATE tessera_rhi_shared)

add_test(NAME retire_queue COMMAND retire_queue_tests)

# The RHI -> Metal mapping tables (ADR-053). API-free, so they run on every host; the device half
# is Apple-only and gated in src/rendering/backend/metal/CMakeLists.txt.
add_executable(metal_mapping_tests
    metal_mapping_tests.cpp)

target_link_libraries(metal_mapping_tests PRIVATE tessera_metal_mapping)

add_test(NAME metal_mapping COMMAND metal_mapping_tests)

# The MSL shader table (ADR-053 decision 2). API-free; the table is populated only when the build
# embedded cross-compiled MSL, and both states are covered.
add_executable(metal_shader_registry_tests
    metal_shader_registry_tests.cpp)

target_link_libraries(metal_shader_registry_tests PRIVATE tessera_metal_mapping)

add_test(NAME metal_shader_registry COMMAND metal_shader_registry_tests)

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
# App-level test support (issue #454): the `tessera_testing` kit's own contract and its acceptance
# tests. Own directory for the same reason as the others: one `add_subdirectory` here keeps this file
# the single index while the slice owns its own registrations.
add_subdirectory(testing)

# ICU-backed text tests exist only when the reduced-data option is on (CI keeps it off).
if (TESSERA_ICU_REDUCED_DATA)
    add_subdirectory(icu)
endif()

add_executable(css_module_isolation_tests
    css_module_isolation_tests.cpp)

target_link_libraries(css_module_isolation_tests PRIVATE tessera_css_module tessera_style)

add_test(NAME css_module_isolation COMMAND css_module_isolation_tests)

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

# ADR-051 slice 5: record values in the evaluator. Split out of ir_eval_tests.cpp when that file
# crossed the 500-line hard limit.
add_executable(ir_eval_record_tests
    ir_eval_record_tests.cpp)

target_link_libraries(ir_eval_record_tests PRIVATE tessera_ir_eval)

add_test(NAME ir_eval_record COMMAND ir_eval_record_tests)

# ADR-067 spike #661: the pending-UI model on its own (Suspense fallback/restore/keep-on-refresh,
# the transition mark's isPending, the stale-epoch drop). Header-only, so it links nothing.
add_executable(pending_model_tests
    pending_model_tests.cpp)

target_include_directories(pending_model_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_test(NAME pending_model COMMAND pending_model_tests)

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

# Stable component identity (docs/specs/automation-ids-v1.md section 2.1): insertion/reorder of
# components keeps or intentionally moves ids, and inserting a library component keeps the
# generated C++ of the untouched components byte-identical.
add_executable(ir_ids_stability_tests
    ir_ids_stability_tests.cpp)

target_link_libraries(ir_ids_stability_tests PRIVATE
    tessera_ir_ids
    tessera_lowering
    tessera_codegen)

target_compile_definitions(ir_ids_stability_tests PRIVATE
    TESSERA_LIBRARY_TSX_DIR="${CMAKE_SOURCE_DIR}/packages/tessera-ui")

add_test(NAME ir_ids_stability COMMAND ir_ids_stability_tests)

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

# The adaptive render-scale extent consumer (docs/design/performance.md 6.1): the presented extent,
# the quantized device (glyph) scale and the controller's adaptive multiplier turn into the render
# target extent, with fractional rounding, the 1-pixel floor and exact identity at scale 1.0. Pure
# CPU: no GPU, window or clock.
add_executable(render_target_extent_tests
    render_target_extent_tests.cpp)

target_link_libraries(render_target_extent_tests PRIVATE tessera_scale)

add_test(NAME render_target_extent COMMAND render_target_extent_tests)

# --- Inline spans in Text (issue #229) ---------------------------------------------------------
add_executable(lowering_inline_span_tests
    lowering_inline_span_tests.cpp)
target_link_libraries(lowering_inline_span_tests PRIVATE tessera_lowering)
add_test(NAME lowering_inline_span COMMAND lowering_inline_span_tests)

add_executable(ir_inline_span_tests
    ir_inline_span_tests.cpp
    ir_samples.cpp)
target_link_libraries(ir_inline_span_tests PRIVATE tessera_ir)
add_test(NAME ir_inline_span COMMAND ir_inline_span_tests)

add_executable(paint_text_span_tests
    paint_text_span_tests.cpp)
target_link_libraries(paint_text_span_tests PRIVATE tessera_paint tessera_scene)
add_test(NAME paint_text_span COMMAND paint_text_span_tests)

add_executable(a11y_span_tests
    a11y_span_tests.cpp)
target_link_libraries(a11y_span_tests PRIVATE tessera_accessibility)
add_test(NAME a11y_span COMMAND a11y_span_tests)

add_executable(codegen_span_tests
    codegen_span_tests.cpp)
target_link_libraries(codegen_span_tests PRIVATE tessera_codegen)
add_test(NAME codegen_span COMMAND codegen_span_tests)

# Issue #286: a callback prop forwarded to an event (`onClick={onPress}`) lowers to a `callProp`
# action, validates, and generates a guarded call of the prop's std::function.
add_executable(lowering_callback_forward_tests
    lowering_callback_forward_tests.cpp)
target_link_libraries(lowering_callback_forward_tests PRIVATE tessera_lowering tessera_codegen tessera_ir_ids)
target_compile_definitions(lowering_callback_forward_tests PRIVATE
    TESSERA_CODEGEN_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/codegen")
add_test(NAME lowering_callback_forward COMMAND lowering_callback_forward_tests)

# Issue #291: an imported component shadows an intrinsic element of the same name.
add_executable(lowering_import_shadow_tests
    lowering_import_shadow_tests.cpp)
target_link_libraries(lowering_import_shadow_tests PRIVATE tessera_lowering)
add_test(NAME lowering_import_shadow COMMAND lowering_import_shadow_tests)

# Chart series sample storage (issue #182, docs/specs/chart-series-v1.md): fixed capacity,
# overwrite-oldest, logical oldest -> newest iteration. The test replaces the global operator
# new/delete (the technique tests/benchmarks/stylesheet/benchmark_counters.cpp uses) and proves that
# appends, reads, iteration and clear allocate zero blocks and zero bytes after construction.
add_executable(chart_series_ring_tests
    chart_series_ring_tests.cpp
    chart_series_ring_allocation.cpp)

target_include_directories(chart_series_ring_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include)

# Replaces the global operator new/delete to count allocations; see the helper's comment.
tessera_allow_allocator_override(chart_series_ring_tests)

add_test(NAME chart_series_ring COMMAND chart_series_ring_tests)

# Issue #239: a `{children}` Slot in a salted project gets a uiId and no automation id.
add_executable(ir_ids_slot_tests
    ir_ids_slot_tests.cpp)
target_link_libraries(ir_ids_slot_tests PRIVATE tessera_lowering tessera_ir_ids)
add_test(NAME ir_ids_slot COMMAND ir_ids_slot_tests)

# Issue #239: the Button stylesheet compiles cleanly and reads only tokens every Instrument theme
# defines.
add_executable(library_button_style_tests
    library_button_style_tests.cpp)
target_link_libraries(library_button_style_tests PRIVATE tessera_style)
target_compile_definitions(library_button_style_tests PRIVATE
    TESSERA_BUTTON_CSS="${CMAKE_SOURCE_DIR}/packages/tessera-ui/button.css"
    TESSERA_INSTRUMENT_THEME="${CMAKE_SOURCE_DIR}/packages/tessera-ui/themes/instrument.theme.json")
add_test(NAME library_button_style COMMAND library_button_style_tests)

# Issue #239: the library Button lowers, validates under a salt and generates C++.
add_executable(library_button_lowering_tests
    library_button_lowering_tests.cpp)
target_link_libraries(library_button_lowering_tests PRIVATE
    tessera_lowering tessera_codegen tessera_ir_ids)
target_compile_definitions(library_button_lowering_tests PRIVATE
    TESSERA_BUTTON_TSX="${CMAKE_SOURCE_DIR}/packages/tessera-ui/Button.tsx"
    TESSERA_CODEGEN_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/codegen")
add_test(NAME library_button_lowering COMMAND library_button_lowering_tests)

# SafeArea in the app host (#223): resolved insets become padding or margin before layout, per-edge
# keyboard easing under the manual clock, reduced motion, and no layout for unchanged insets.
add_executable(app_safe_area_tests
    app_safe_area_tests.cpp)

target_link_libraries(app_safe_area_tests PRIVATE tessera_app tessera_runtime)

add_test(NAME app_safe_area COMMAND app_safe_area_tests)

# Issue #302: overlay layers, handles, hit order, modal scrim/focus/semantics through a real AppHost.
add_executable(overlay_host_tests
    overlay_host_tests.cpp)

target_link_libraries(overlay_host_tests PRIVATE tessera_app tessera_runtime tessera_accessibility)

add_test(NAME overlay_host COMMAND overlay_host_tests)

# Issue #302 slice 2: placement math, anchored placement, dismissal and the input hook.
add_executable(overlay_placement_tests
    overlay_placement_tests.cpp)

target_link_libraries(overlay_placement_tests PRIVATE tessera_scene)

add_test(NAME overlay_placement COMMAND overlay_placement_tests)

add_executable(overlay_dismiss_tests
    overlay_dismiss_tests.cpp)

target_link_libraries(overlay_dismiss_tests PRIVATE tessera_app tessera_runtime tessera_accessibility
    tessera_input_pump)

add_test(NAME overlay_dismiss COMMAND overlay_dismiss_tests)

# System buttons as normalized events (issue #445, docs/specs/studio-device-profiles-v1.md section 4):
# Back dispatch (overlay, route, unhandled), the Home/Recents/Power mappings, Volume delivery, the
# section 3 ordering, and the saved-state record. Deterministic: the synthetic injector and
# `ManualClock` only.
add_executable(system_buttons_tests
    system_buttons_tests.cpp)

target_include_directories(system_buttons_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/tests/unit)

target_link_libraries(system_buttons_tests PRIVATE tessera_app tessera_runtime tessera_accessibility
    tessera_input_pump tessera_appfw_navigation tessera_hal_window)

add_test(NAME system_buttons COMMAND system_buttons_tests)

# Issue #223: the library SafeArea lowers, validates under a salt and generates C++.
add_executable(library_safe_area_lowering_tests
    library_safe_area_lowering_tests.cpp)
target_link_libraries(library_safe_area_lowering_tests PRIVATE
    tessera_lowering tessera_codegen tessera_ir_ids)
target_compile_definitions(library_safe_area_lowering_tests PRIVATE
    TESSERA_SAFE_AREA_TSX="${CMAKE_SOURCE_DIR}/packages/tessera-ui/SafeArea.tsx"
    TESSERA_CODEGEN_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/codegen")
add_test(NAME library_safe_area_lowering COMMAND library_safe_area_lowering_tests)

# Border widths snap to whole device pixels at paint time (display-aware-rendering.md S3).
add_executable(paint_border_snap_tests
    paint_border_snap_tests.cpp)

target_link_libraries(paint_border_snap_tests PRIVATE tessera_paint)

add_test(NAME paint_border_snap COMMAND paint_border_snap_tests)

# CSS `text-decoration` and `vertical-align` (docs/design/text-decoration.md): value grammar,
# registry/shorthand/@supports, the computed-value mapping, and the painted rules and baseline shift.
add_executable(text_decoration_tests
    text_decoration_tests.cpp)
target_link_libraries(text_decoration_tests PRIVATE tessera_paint tessera_text tessera_style)
add_test(NAME text_decoration COMMAND text_decoration_tests)

# The dense per-generation style memo (issue #735, slice G): reference stability across resolves,
# `keep_entries`, the restyle digest record, the outside-the-tree answer and the container-source
# bypass. Registered here, appended, so no existing entry moves.
add_executable(scene_style_memo_tests
    scene_style_memo_tests.cpp)
target_link_libraries(scene_style_memo_tests PRIVATE tessera_style)
add_test(NAME scene_style_memo COMMAND scene_style_memo_tests)
