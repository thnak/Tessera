# SPDX-License-Identifier: MIT

add_executable(rhi_null_tests
    rhi/rhi_null_tests.cpp)
target_link_libraries(rhi_null_tests PRIVATE tessera_rhi_null)
add_test(NAME rhi_null COMMAND rhi_null_tests)

# RHI 1.17 `copy_buffer_to_texture` (ADR-072) on the null backend: the validation matrix, the record and
# the command order. The pixels are read back by buffer_texture_copy_tests on a real device.
add_executable(buffer_texture_copy_null_tests
    rhi/buffer_texture_copy_null_tests.cpp)
target_link_libraries(buffer_texture_copy_null_tests PRIVATE tessera_rhi_null)
add_test(NAME buffer_texture_copy_null COMMAND buffer_texture_copy_null_tests)

add_executable(rhi_buffer_bounds_tests
    rhi/rhi_buffer_bounds_tests.cpp)
target_link_libraries(rhi_buffer_bounds_tests PRIVATE tessera_rhi_null)
if (TARGET tessera_rhi_vulkan)
    target_link_libraries(rhi_buffer_bounds_tests PRIVATE tessera_rhi_vulkan)
    target_compile_definitions(rhi_buffer_bounds_tests PRIVATE TESSERA_TEST_HAS_VULKAN)
endif()
add_test(NAME rhi_buffer_bounds COMMAND rhi_buffer_bounds_tests)

add_executable(rhi_memory_tests
    rhi/rhi_memory_tests.cpp)
target_link_libraries(rhi_memory_tests PRIVATE tessera_rhi_null)
if (TARGET tessera_rhi_vulkan)
    target_link_libraries(rhi_memory_tests PRIVATE tessera_rhi_vulkan)
    target_compile_definitions(rhi_memory_tests PRIVATE TESSERA_TEST_HAS_VULKAN)
endif()
add_test(NAME rhi_memory COMMAND rhi_memory_tests)

# RHI 1.5 dual-source blend contract (issue #471 slice 1): `BlendMode::DualSource` plus
# `Device::supports_dual_source_blend()`. The null half runs everywhere and asserts the recorded
# blend mode; the Vulkan half creates a DualSource pipeline on a device that reports the feature and
# asserts the request is rejected on one that does not. A device without `dualSrcBlend` prints an
# explicit skip (ctest SKIP_REGULAR_EXPRESSION) rather than passing without creating anything.
# WebGPU's rejection is pinned in tests/unit/webgpu_mapping_tests.cpp, the host-buildable half of
# that backend.
add_executable(dual_source_blend_tests
    rhi/dual_source_blend_tests.cpp)
target_link_libraries(dual_source_blend_tests PRIVATE tessera_rhi_null)
if (TARGET tessera_rhi_vulkan)
    target_link_libraries(dual_source_blend_tests PRIVATE tessera_rhi_vulkan)
    target_compile_definitions(dual_source_blend_tests PRIVATE TESSERA_TEST_HAS_VULKAN)
endif()
add_test(NAME dual_source_blend COMMAND dual_source_blend_tests)
set_tests_properties(dual_source_blend PROPERTIES
    SKIP_REGULAR_EXPRESSION "skipped"
    FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

add_executable(batched2d_tests
    batched2d/batched2d_tests.cpp)
target_link_libraries(batched2d_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME batched2d COMMAND batched2d_tests)

add_executable(frames_in_flight_tests
    batched2d/frames_in_flight_tests.cpp)
target_link_libraries(frames_in_flight_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME frames_in_flight COMMAND frames_in_flight_tests)

# Isolated-layer seam (#242): render-pass break/copy/resume recording and the bounded offscreen
# target pool. Null backend, deterministic, runs everywhere.
add_executable(renderer_seam_tests
    batched2d/renderer_seam_tests.cpp)
target_link_libraries(renderer_seam_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME renderer_seam COMMAND renderer_seam_tests)

# The renderer's pipeline axis for LCD text (#471): batch identity, replay binds, restore.
add_executable(lcd_pipeline_axis_tests
    batched2d/lcd_pipeline_axis_tests.cpp)
target_link_libraries(lcd_pipeline_axis_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME lcd_pipeline_axis COMMAND lcd_pipeline_axis_tests)

# RHI 1.3 sampled render target (#254): the command order the glass composite records
# (attachment -> copy -> sample -> composite), the sampler state that travels with a target and the
# misuse every backend rejects. Null backend, deterministic, runs everywhere. The pixel proof is
# glass_composite_gpu below.
add_executable(sampled_target_tests
    batched2d/sampled_target_tests.cpp)
target_link_libraries(sampled_target_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME sampled_target COMMAND sampled_target_tests)

# Glass composite (#244) through the real paint path on the null backend: paint_scene -> Batched2DSink
# -> Batched2DRenderer::draw_backdrop records break -> copy -> two blur passes -> composite, and the
# bounded fallbacks (no pipelines, exhausted pool, blur(0)) stay counted instead of throwing. A tree
# without a glass node keeps its single-pass stream. Runs everywhere.
add_executable(glass_paint_null_tests
    batched2d/glass_paint_null_tests.cpp)
target_link_libraries(glass_paint_null_tests PRIVATE
    tessera_gpu tessera_paint tessera_rhi_null)
add_test(NAME glass_paint_null COMMAND glass_paint_null_tests)

# Isolated layers (issue #227 slice 2) on the null backend: the pass and draw skeleton of one layer,
# nesting, the bounded pool, and the flat fallbacks. Runs everywhere.
add_executable(layer_null_tests
    batched2d/layer_null_tests.cpp)
target_link_libraries(layer_null_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME layer_null COMMAND layer_null_tests)

# Layer masks (issue #617 slice 2) on the null backend: the pass and draw skeleton of a masked
# layer and every fallback. Runs everywhere.
# Per-fragment gradients (ADR-076) on the null backend: one quad per layer against the banded
# fallback (counted), the capability fallback, culling, the table limit and batching.
add_executable(gradient_null_tests
    batched2d/gradient_null_tests.cpp)
target_link_libraries(gradient_null_tests PRIVATE tessera_gpu tessera_paint tessera_rhi_null)
add_test(NAME gradient_null COMMAND gradient_null_tests)

add_executable(layer_mask_null_tests
    batched2d/layer_mask_null_tests.cpp)
target_link_libraries(layer_mask_null_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME layer_mask_null COMMAND layer_mask_null_tests)
# The offscreen target pool itself (offscreen-layers.md section 5e): in-frame and cross-frame
# release, retirement, both caps, idle trim, zero steady-state allocation. Runs everywhere.
add_executable(offscreen_pool_tests
    batched2d/offscreen_pool_tests.cpp)
target_link_libraries(offscreen_pool_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME offscreen_pool COMMAND offscreen_pool_tests)
add_executable(layer_pool_null_tests
    batched2d/layer_pool_null_tests.cpp)
target_link_libraries(layer_pool_null_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME layer_pool_null COMMAND layer_pool_null_tests)
# Retained layers (issue #617, offscreen-layers.md section 5.6): record once, composite while
# unchanged, invalidate, damage, budgets and trim, on the null backend. Runs everywhere.
add_executable(layer_retain_null_tests
    batched2d/layer_retain_null_tests.cpp)
target_link_libraries(layer_retain_null_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME layer_retain_null COMMAND layer_retain_null_tests)
add_executable(layer_filter_null_tests
    batched2d/layer_filter_null_tests.cpp)
target_link_libraries(layer_filter_null_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME layer_filter_null COMMAND layer_filter_null_tests)

# What a layer `filter` needs from the window's frame target: nothing (no offscreen frame target, no
# region copy or resample of it), unlike glass. Counters before/after on a filter-only scene.
add_executable(filter_frame_target_tests
    batched2d/filter_frame_target_tests.cpp)
target_link_libraries(filter_frame_target_tests PRIVATE
    tessera_gpu tessera_paint tessera_rhi_null)
add_test(NAME filter_frame_target COMMAND filter_frame_target_tests)

add_executable(damage_redraw_tests
    batched2d/damage_redraw_tests.cpp)
target_link_libraries(damage_redraw_tests PRIVATE tessera_gpu tessera_rhi_null tessera_damage)
add_test(NAME damage_redraw COMMAND damage_redraw_tests)

add_executable(cull_tests
    batched2d/cull_tests.cpp)
target_link_libraries(cull_tests PRIVATE tessera_gpu tessera_rhi_null tessera_damage)
add_test(NAME cull COMMAND cull_tests)

# Batch-split attribution (issue #480 part 2): why a frame's batches ended, so the texture share
# descriptor indexing would remove is a measured number instead of a guess. Null backend.
add_executable(batch_split_tests
    batched2d/batch_split_tests.cpp)
target_link_libraries(batch_split_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME batch_split COMMAND batch_split_tests)

# Texture-array fallback (RHI 1.7, issue #480 part 2): a device without descriptor indexing reports
# capacity 0, refuses the array pipeline and the array bind by name, and keeps the one-set path.
add_executable(texture_array_fallback_tests
    batched2d/texture_array_fallback_tests.cpp)
target_link_libraries(texture_array_fallback_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME texture_array_fallback COMMAND texture_array_fallback_tests)

set(TESSERA_FONT_FIXTURE
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/NotoSans-Regular.ttf")
get_filename_component(TESSERA_FONT_FIXTURE "${TESSERA_FONT_FIXTURE}" REALPATH)

set(TESSERA_BIDI_FONT_FIXTURE
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/NotoSansHebrew-Regular.ttf")
get_filename_component(TESSERA_BIDI_FONT_FIXTURE "${TESSERA_BIDI_FONT_FIXTURE}" REALPATH)

# ADR-049 drift guard: the batched2d GLSL (embedded as text, no glslc needed) and the embedded WGSL
# twin declare the same interface. Runs in every build, Android included, because the GLSL is read
# on the host at configure time and shipped in the executable: the test reads no source-tree paths
# (issue #92). The tool-backed checks are opt-in (TESSERA_SHADER_CROSS, tools/shaders).
add_executable(shader_parity_tests
    shaders/shader_parity_tests.cpp)
target_link_libraries(shader_parity_tests PRIVATE tessera_gpu tessera_webgpu_mapping)

# Embed the authored GLSL at configure time. configure_file re-runs automatically when either
# shader changes (CMAKE_CONFIGURE_DEPENDS), so the embedded copy cannot drift from the sources.
set(TESSERA_BATCHED2D_GLSL_DIR
    "${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/shaders")
file(READ "${TESSERA_BATCHED2D_GLSL_DIR}/batched2d.vert" TESSERA_BATCHED2D_VERT_GLSL)
file(READ "${TESSERA_BATCHED2D_GLSL_DIR}/batched2d.frag" TESSERA_BATCHED2D_FRAG_GLSL)
# The LCD variant's fragment stage (issue #471), embedded so the parity test can pin that its
# clip/SDF/shadow logic cannot drift from batched2d.frag (ADR-068 slice 3 moved both to the clip
# record buffer, and a divergence there would be invisible until a golden moved).
file(READ "${TESSERA_BATCHED2D_GLSL_DIR}/batched2d_lcd.frag" TESSERA_BATCHED2D_LCD_FRAG_GLSL)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${TESSERA_BATCHED2D_GLSL_DIR}/batched2d.vert"
    "${TESSERA_BATCHED2D_GLSL_DIR}/batched2d.frag"
    "${TESSERA_BATCHED2D_GLSL_DIR}/batched2d_lcd.frag")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
# LF output so the embedded text is byte-identical to the authored files (the repo stores shaders
# with LF, .gitattributes `eol=lf`) and the generated header is the same on every host.
configure_file(shaders/batched2d_glsl_sources.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/batched2d_glsl_sources.hpp" @ONLY NEWLINE_STYLE LF)
target_include_directories(shader_parity_tests PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
if (TARGET tessera_rhi_vulkan)
    target_link_libraries(shader_parity_tests PRIVATE tessera_rhi_vulkan)
    target_compile_definitions(shader_parity_tests PRIVATE TESSERA_TEST_HAS_VULKAN)
endif()
add_test(NAME shader_parity COMMAND shader_parity_tests)

add_executable(text_render_tests
    text/text_render_tests.cpp)
target_link_libraries(text_render_tests PRIVATE tessera_text_render tessera_rhi_null)
add_test(NAME text_render COMMAND text_render_tests "${TESSERA_FONT_FIXTURE}")

add_executable(text_render_faces_tests
    text/text_render_faces_tests.cpp)
target_link_libraries(text_render_faces_tests PRIVATE tessera_text_render tessera_rhi_null)
add_test(NAME text_render_faces COMMAND text_render_faces_tests
    "${TESSERA_FONT_FIXTURE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/NotoSansHebrew-Regular.ttf")

# Path raster cache -> batched2d textured draw. The null-device section runs everywhere; the Vulkan
# readback cases are compiled in only when the Vulkan backend is part of the build.
add_executable(path_texture_draw_tests
    batched2d/path_texture_draw_tests.cpp)
target_link_libraries(path_texture_draw_tests PRIVATE
    tessera_gpu tessera_path tessera_rhi_null)
target_compile_definitions(path_texture_draw_tests PRIVATE
    TESSERA_SVG_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/svg")
if (TARGET tessera_rhi_vulkan)
    target_link_libraries(path_texture_draw_tests PRIVATE tessera_rhi_vulkan)
    target_compile_definitions(path_texture_draw_tests PRIVATE TESSERA_TEST_HAS_VULKAN)
endif()
add_test(NAME path_texture_draw COMMAND path_texture_draw_tests)
set_tests_properties(path_texture_draw PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

# Integer-exact nearest sampling (issue #682): push_texture_exact must equal the CPU rule byte for
# byte where the hardware nearest sampler is not exact (2:1 and fractional ratios). Vulkan only; the
# test skips when no device is available.
if (TARGET tessera_rhi_vulkan)
    add_executable(texture_exact_tests
        batched2d/texture_exact_tests.cpp)
    target_link_libraries(texture_exact_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME texture_exact COMMAND texture_exact_tests)
endif()

# Manual measurement tool (not a CTest): path upload / cache-hit / SVG decode cost on a real device.
if (TARGET tessera_rhi_vulkan)
    add_executable(path_texture_measure
        batched2d/path_texture_measure.cpp)
    target_link_libraries(path_texture_measure PRIVATE
        tessera_gpu tessera_path tessera_rhi_null tessera_rhi_vulkan)
    target_compile_definitions(path_texture_measure PRIVATE
        TESSERA_SVG_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/svg")

    # Manual measurement tool (not a CTest): where the CPU and GPU route of PathTextureCache cross
    # over, per fixture and size (ADR-071).
    add_executable(path_route_measure
        batched2d/path_route_measure.cpp)
    target_link_libraries(path_route_measure PRIVATE
        tessera_gpu tessera_path tessera_rhi_null tessera_rhi_vulkan)
    target_compile_definitions(path_route_measure PRIVATE
        TESSERA_SVG_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/svg")

    # Manual measurement tool (not a CTest): CPU raster + upload cost at 4K, the numbers behind the
    # decision to move vector paths and other CPU-produced pixels onto the GPU.
    add_executable(cpu_to_gpu_4k_measure
        batched2d/cpu_to_gpu_4k_measure.cpp
        batched2d/cpu_to_gpu_4k_hypotheses.cpp
        batched2d/cpu_to_gpu_4k_create_texture.cpp)
    target_link_libraries(cpu_to_gpu_4k_measure PRIVATE
        tessera_gpu tessera_path tessera_image tessera_rhi_vulkan)
endif()

# Manual measurement tool (not a CTest): the number issue #480 part 2 is gated on. It paints an
# interleaved image+text scene through the production paint path and reports the batch-split
# attribution, so the texture share descriptor indexing would remove is measured instead of guessed.
# The raster-image fixture is embedded, so the tool needs no runtime filesystem.
include(${PROJECT_SOURCE_DIR}/cmake/TesseraAssets.cmake)
add_executable(texture_batch_measure
    batched2d/texture_batch_measure.cpp)
tessera_embed_assets(texture_batch_measure
    MANIFEST ${PROJECT_SOURCE_DIR}/tests/fixtures/assets/images.manifest
    NAMESPACE tessera::test::texture_batch_measure::fixtures
    STEM texture_batch_measure)
target_link_libraries(texture_batch_measure PRIVATE
    tessera_gpu tessera_paint tessera_text_render tessera_image tessera_asset_image
    tessera_rhi_null)
if (TARGET tessera_rhi_vulkan)
    target_link_libraries(texture_batch_measure PRIVATE tessera_rhi_vulkan)
    target_compile_definitions(texture_batch_measure PRIVATE TESSERA_TEST_HAS_VULKAN)
endif()

# Backend-selectable RHI conformance suite plus its deterministic summary (ADR-049, issue #161).
add_subdirectory(conformance)

# Per-test golden tolerances: tests/golden/tolerance.json is the single source, resolved here at
# configure time through scripts/golden_tolerances.py. A test without an explicit entry inherits the
# file's "defaults" block, and an unknown/malformed file fails the configure step instead of
# silently loosening what a golden accepts.
if (NOT Python3_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
set(TESSERA_GOLDEN_TOLERANCE_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/golden_tolerances.py")
function(tessera_golden_tolerances name tolerance_var max_mismatch_var)
    foreach(field tolerance max_mismatch)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -B "${TESSERA_GOLDEN_TOLERANCE_SCRIPT}"
                --name "${name}" --field "${field}"
            OUTPUT_VARIABLE value
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE result)
        if (NOT result EQUAL 0)
            message(FATAL_ERROR
                "tolerance.json: cannot resolve ${field} for '${name}' (${result}): ${value}")
        endif()
        if (field STREQUAL "tolerance")
            set(tolerance "${value}")
        else()
            set(max_mismatch "${value}")
        endif()
    endforeach()
    set(${tolerance_var} "${tolerance}" PARENT_SCOPE)
    set(${max_mismatch_var} "${max_mismatch}" PARENT_SCOPE)
endfunction()

# Cross-backend image parity harness (issue #245); the directory owns its targets and ctest entries.
add_subdirectory(backends)

# AF6 (issue #178, ADR-050 section 7): native surfaces and the renderer that composites into one.
# The directory owns its targets and ctest entries, so this lane adds one line here and no shared
# registration.
add_subdirectory(surfaces)

if (TARGET batched2d_poc)
    tessera_golden_tolerances(batched2d_cpu_golden
        TESSERA_TOL_CPU_GOLDEN TESSERA_MAX_CPU_GOLDEN)
    tessera_golden_tolerances(batched2d_cpu_text_golden
        TESSERA_TOL_CPU_TEXT TESSERA_MAX_CPU_TEXT)
    tessera_golden_tolerances(batched2d_golden TESSERA_TOL_BATCHED2D TESSERA_MAX_BATCHED2D)
    tessera_golden_tolerances(batched2d_overlay_golden
        TESSERA_TOL_OVERLAY TESSERA_MAX_OVERLAY)
    tessera_golden_tolerances(batched2d_text_golden TESSERA_TOL_TEXT TESSERA_MAX_TEXT)
    tessera_golden_tolerances(batched2d_shapes_golden
        TESSERA_TOL_SHAPES TESSERA_MAX_SHAPES)
    tessera_golden_tolerances(batched2d_shadow_golden
        TESSERA_TOL_SHADOW TESSERA_MAX_SHADOW)
endif()

if (TARGET batched2d_poc)
    add_test(
        NAME batched2d_vulkan_smoke
        COMMAND batched2d_poc --backend vulkan --width 128 --height 128 --golden
            --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_smoke.png)
    add_test(
        NAME batched2d_vulkan_validation
        COMMAND batched2d_poc --backend vulkan --width 128 --height 128 --validation --golden
            --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_validation.png)
endif()

if (WIN32 AND TARGET batched2d_poc AND TARGET tessera_rhi_vulkan)
    add_test(
        NAME batched2d_windowed_smoke
        COMMAND batched2d_poc --backend vulkan --windowed --frames 3 --width 256 --height 256)
    # A run that renders nothing still exits 0, so require the three frames (or an explicit skip).
    set_tests_properties(batched2d_windowed_smoke PROPERTIES
        PASS_REGULAR_EXPRESSION "rendered=3 |skipped")
    # Scene-driven render-on-demand: only startup and the two scripted scene changes may render,
    # and the loop must idle between them. PASS_REGULAR_EXPRESSION ignores the exit code, so the
    # expression carries the whole check.
    add_test(
        NAME batched2d_on_demand
        COMMAND batched2d_poc --backend vulkan --on-demand --validation --width 320 --height 240)
    # Validation must stay clean: the on-demand path uses LoadOp::Load on swapchain images, which
    # caught a missing layout transition (PRESENT_SRC assumed for a freshly created image).
    set_tests_properties(batched2d_on_demand PROPERTIES
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]"
        PASS_REGULAR_EXPRESSION "on-demand: rendered=([3-9]|[1-9][0-9]+) presented=[0-9]+ scripted=2 events=[0-9]+ idle_waits=[1-9][0-9]* unjustified=0 missed=0|skipped")
endif()

if (TARGET batched2d_poc)
    add_test(
        NAME batched2d_cpu_golden
        COMMAND batched2d_poc --backend cpu --tolerance ${TESSERA_TOL_CPU_GOLDEN}
            --max-mismatch ${TESSERA_MAX_CPU_GOLDEN}
            --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_cpu_golden.png
            --compare ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/dashboard_cpu.png)

    add_test(
        NAME batched2d_cpu_text_golden
        COMMAND batched2d_poc --backend cpu --cpu-text --font ${TESSERA_FONT_FIXTURE}
            --text "Tessera CPU text" --text-size 20 --width 320 --height 160
            --tolerance ${TESSERA_TOL_CPU_TEXT} --max-mismatch ${TESSERA_MAX_CPU_TEXT}
            --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_cpu_text_golden.png
            --compare
            ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/dashboard_text_cpu.png)
endif()

# Wider CPU paint corpus: borders, nested clips and linear gradients painted by the shared paint pass
# into the deterministic CPU sink. Vulkan-free, so it runs on every platform; the committed baselines
# under tests/golden/baseline/cpu_wide/ are compared bit-exactly and regenerated only through
# `cpu_wide_golden_tests --update-goldens` on the CPU path.
add_executable(cpu_wide_golden_tests
    cpu_wide_golden_tests.cpp
    cpu_wide_gradient_scenes.cpp
    cpu_wide_mask_scenes.cpp
    cpu_wide_stop_scenes.cpp
    cpu_wide_rounded_scenes.cpp)
target_include_directories(cpu_wide_golden_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
target_compile_definitions(cpu_wide_golden_tests PRIVATE
    TESSERA_CPU_WIDE_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/baseline/cpu_wide")
target_link_libraries(cpu_wide_golden_tests PRIVATE
    tessera_paint
    tessera_software
    tessera_golden
    tessera_stb)
add_test(NAME cpu_wide_golden COMMAND cpu_wide_golden_tests)

# The CPU twin of isolated layers and layer masks through `CpuPaintSink` (issue #617): the sink calls,
# the mask's refusals and the paint pass's group opacity. Vulkan-free.
add_executable(cpu_layer_sink_tests
    cpu_layer_sink_tests.cpp)
target_include_directories(cpu_layer_sink_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
target_link_libraries(cpu_layer_sink_tests PRIVATE
    tessera_paint
    tessera_software)
add_test(NAME cpu_layer_sink COMMAND cpu_layer_sink_tests)

# Exact rounded corners of the CPU sink (issue #617): a rounded rect, a rounded clip and a
# gradient's rounded border box against an independent circle test, plus the anti-aliased mode.
add_executable(cpu_rounded_sink_tests
    cpu_rounded_sink_tests.cpp)
target_include_directories(cpu_rounded_sink_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
target_link_libraries(cpu_rounded_sink_tests PRIVATE
    tessera_paint
    tessera_software)
add_test(NAME cpu_rounded_sink COMMAND cpu_rounded_sink_tests)

if (UNIX AND TARGET batched2d_poc)
    find_file(TESSERA_LAVAPIPE_ICD
        NAMES lvp_icd.x86_64.json lvp_icd.json
        PATHS /usr/share/vulkan/icd.d /etc/vulkan/icd.d)

    if (TESSERA_LAVAPIPE_ICD)
        add_test(
            NAME batched2d_golden
            COMMAND batched2d_poc --backend vulkan --golden --tolerance ${TESSERA_TOL_BATCHED2D}
                --max-mismatch ${TESSERA_MAX_BATCHED2D}
                --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_golden.png
                --compare ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/dashboard.png)
        set_tests_properties(batched2d_golden PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}")

        add_test(
            NAME batched2d_overlay_golden
            COMMAND batched2d_poc --backend vulkan --golden --overlay
                --font ${TESSERA_FONT_FIXTURE} --tolerance ${TESSERA_TOL_OVERLAY}
                --max-mismatch ${TESSERA_MAX_OVERLAY}
                --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_overlay_golden.png
                --compare ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/dashboard_overlay.png)
        set_tests_properties(batched2d_overlay_golden PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}")

        add_test(
            NAME batched2d_text_golden
            COMMAND batched2d_poc --backend vulkan --golden
                --font ${TESSERA_FONT_FIXTURE} --tolerance ${TESSERA_TOL_TEXT}
                --max-mismatch ${TESSERA_MAX_TEXT}
                --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_text_golden.png
                --compare ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/dashboard_text.png)
        set_tests_properties(batched2d_text_golden PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}")

        add_test(
            NAME batched2d_shapes_golden
            COMMAND batched2d_poc --backend vulkan --golden --shapes --tolerance ${TESSERA_TOL_SHAPES}
                --max-mismatch ${TESSERA_MAX_SHAPES}
                --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_shapes_golden.png
                --compare ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/dashboard_shapes.png)
        set_tests_properties(batched2d_shapes_golden PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}")

        # An outer box-shadow is painted only outside the border box (issue #461), so a transparent
        # box shows the page through its own (rounded) box. The baseline is lavapipe-only and is
        # generated on a Linux lane with:
        #   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
        #   ./build/dev/tools/poc/batched2d/batched2d_poc --backend vulkan --golden --shadow-clip \
        #       --width 320 --height 200 \
        #       --out tests/golden/baseline/batched2d/shadow_clip.png
        add_test(
            NAME batched2d_shadow_golden
            COMMAND batched2d_poc --backend vulkan --golden --shadow-clip --width 320 --height 200
                --tolerance ${TESSERA_TOL_SHADOW} --max-mismatch ${TESSERA_MAX_SHADOW}
                --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_shadow_golden.png
                --compare ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/shadow_clip.png)
        set_tests_properties(batched2d_shadow_golden PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}")

        add_test(
            NAME batched2d_windowed_smoke
            COMMAND batched2d_poc --backend vulkan --windowed --frames 3 --width 256 --height 256)
        set_tests_properties(batched2d_windowed_smoke PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}"
            PASS_REGULAR_EXPRESSION "rendered=3 |skipped")
        add_test(
            NAME batched2d_on_demand
            COMMAND batched2d_poc --backend vulkan --on-demand --validation --width 320 --height 240)
        set_tests_properties(batched2d_on_demand PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}"
            FAIL_REGULAR_EXPRESSION "\\[vulkan\\]"
            PASS_REGULAR_EXPRESSION "on-demand: rendered=([3-9]|[1-9][0-9]+) presented=[0-9]+ scripted=2 events=[0-9]+ idle_waits=[1-9][0-9]* unjustified=0 missed=0|skipped")
    endif()
endif()

if (TARGET tessera_rhi_vulkan)
    add_executable(damage_equivalence_tests
        batched2d/damage_equivalence_tests.cpp)
    target_link_libraries(damage_equivalence_tests PRIVATE
        tessera_gpu tessera_damage tessera_golden tessera_rhi_vulkan)
    add_test(NAME damage_equivalence COMMAND damage_equivalence_tests)

    add_executable(rounded_clip_tests
        batched2d/rounded_clip_tests.cpp)
    target_link_libraries(rounded_clip_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME rounded_clip COMMAND rounded_clip_tests)
    set_tests_properties(rounded_clip PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # `clip-path: polygon()` (ADR-068 slice 5, issue #673): every pixel of a polygon-clipped quad is
    # compared with the CPU reference, for convex, concave, self-intersecting (both fill rules),
    # nested and over-limit polygons.
    add_executable(polygon_clip_tests
        batched2d/polygon_clip_tests.cpp)
    target_link_libraries(polygon_clip_tests PRIVATE
        tessera_gpu tessera_paint tessera_software tessera_rhi_vulkan)
    add_test(NAME polygon_clip COMMAND polygon_clip_tests)
    set_tests_properties(polygon_clip PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # An animated `clip-path` (ADR-068 slice 6, issue #674): a polygon morph sampled from keyframes
    # at fixed steps, every frame compared pixel for pixel with the CPU reference.
    add_executable(clip_path_animation_tests
        batched2d/clip_path_animation_tests.cpp)
    target_link_libraries(clip_path_animation_tests PRIVATE
        tessera_gpu tessera_paint tessera_software tessera_animation tessera_rhi_vulkan)
    target_include_directories(clip_path_animation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/batched2d)
    target_include_directories(polygon_clip_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/batched2d)
    add_test(NAME clip_path_animation COMMAND clip_path_animation_tests)
    set_tests_properties(clip_path_animation PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # An outer box-shadow is painted only outside the border box (issue #461): a real readback
    # proves a transparent node shows the page colour inside its own (rounded) box, through the
    # renderer's Gaussian quad and through the paint pass for both the sharp and the blurred path.
    add_executable(shadow_clip_gpu_tests
        batched2d/shadow_clip_gpu_tests.cpp)
    target_link_libraries(shadow_clip_gpu_tests PRIVATE
        tessera_gpu tessera_paint tessera_rhi_vulkan)
    add_test(NAME shadow_clip_gpu COMMAND shadow_clip_gpu_tests)
    set_tests_properties(shadow_clip_gpu PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # The inset box-shadow branch of the fragment shader against Chromium (issue #612): six scenes
    # through the paint pass. The branch was unreachable behind the outer-shadow test (mode > 1000
    # ahead of mode > 2000) until this test caught it. Skips without a Vulkan device.
    add_executable(inset_shadow_gpu_tests
        batched2d/inset_shadow_gpu_tests.cpp)
    target_include_directories(inset_shadow_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/batched2d)
    target_link_libraries(inset_shadow_gpu_tests PRIVATE tessera_gpu tessera_paint tessera_rhi_vulkan)
    add_test(NAME inset_shadow_gpu COMMAND inset_shadow_gpu_tests)
    set_tests_properties(inset_shadow_gpu PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # Isolated-layer seam (#242) pixel test: draw, break the pass, copy a region, resume, draw.
    add_executable(renderer_seam_gpu_tests
        batched2d/renderer_seam_gpu_tests.cpp)
    target_link_libraries(renderer_seam_gpu_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME renderer_seam_gpu COMMAND renderer_seam_gpu_tests)
    set_tests_properties(renderer_seam_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # LCD text compositing (#471): a real readback proves the per-channel blend, the grayscale
    # pipeline's alpha-only read, the no-pipeline fallback and the frame pipeline's restore. Skips
    # on a device without dualSrcBlend.
    add_executable(lcd_text_gpu_tests
        batched2d/lcd_text_gpu_tests.cpp)
    target_link_libraries(lcd_text_gpu_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME lcd_text_gpu COMMAND lcd_text_gpu_tests)
    set_tests_properties(lcd_text_gpu PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # Texture arrays via descriptor indexing (RHI 1.7, issue #480 part 2): an alternating-texture
    # scene records one draw instead of one per switch, and its pixels are byte-identical to the
    # one-set-per-texture path. Skips on a device without the capability.
    add_executable(texture_array_gpu_tests
        batched2d/texture_array_gpu_tests.cpp)
    target_link_libraries(texture_array_gpu_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME texture_array_gpu COMMAND texture_array_gpu_tests)
    set_tests_properties(texture_array_gpu PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # RHI 1.6 mip chain (#491) pixel test: a 1px checkerboard drawn at a 3x downscale aliases to pure
    # black/white with one level and reads mid-grey with a full chain. Skips without a Vulkan
    # device or a format that can be blitted.
    add_executable(texture_mips_gpu_tests
        batched2d/texture_mips_gpu_tests.cpp)
    target_link_libraries(texture_mips_gpu_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME texture_mips_gpu COMMAND texture_mips_gpu_tests)
    set_tests_properties(texture_mips_gpu PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # RHI 1.9 `Texture::write_region` (#681) pixel test: a rectangle written from a pitched and from a
    # tightly packed source lands exactly, nothing outside it moves, a texture filled by bands equals
    # one created whole. Skips without a Vulkan device.
    add_executable(texture_write_region_tests
        batched2d/texture_write_region_tests.cpp)
    target_link_libraries(texture_write_region_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME texture_write_region COMMAND texture_write_region_tests)
    set_tests_properties(texture_write_region PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # RHI 1.17 `copy_buffer_to_texture` (ADR-072): staging, storage and compute-fed sources read back
    # through the pipeline. The compute case reuses the compute probe shader, compiled below, and is
    # compiled out where glslc is absent. Skips without a Vulkan device.
    add_executable(buffer_texture_copy_tests
        batched2d/buffer_texture_copy_tests.cpp)
    target_link_libraries(buffer_texture_copy_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    if (Vulkan_GLSLC_EXECUTABLE)
        target_compile_definitions(buffer_texture_copy_tests PRIVATE TESSERA_HAS_COPY_PROBE)
        target_include_directories(buffer_texture_copy_tests PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated)
        add_dependencies(buffer_texture_copy_tests compute_dispatch_tests)
    endif()
    add_test(NAME buffer_texture_copy COMMAND buffer_texture_copy_tests)
    set_tests_properties(buffer_texture_copy PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # GPU path rasterizer (ADR-071) through the production encoder and pipelines, handed to a texture
    # with copy_buffer_to_texture (ADR-072). Skips without a Vulkan device or without compute.
    add_executable(path_raster_gpu_tests
        batched2d/path_raster_gpu_tests.cpp)
    target_link_libraries(path_raster_gpu_tests PRIVATE tessera_gpu tessera_path tessera_rhi_vulkan)
    add_test(NAME path_raster_gpu COMMAND path_raster_gpu_tests)
    set_tests_properties(path_raster_gpu PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # GpuPathRasterizer batching: several rasters, one submission, slot reuse. Skips like the above.
    add_executable(path_raster_batch_gpu_tests
        batched2d/path_raster_batch_gpu_tests.cpp)
    target_link_libraries(path_raster_batch_gpu_tests PRIVATE tessera_gpu tessera_path tessera_rhi_vulkan)
    add_test(NAME path_raster_batch_gpu COMMAND path_raster_batch_gpu_tests)
    set_tests_properties(path_raster_batch_gpu PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # PathTextureCache with the GPU rasterizer attached: routing, flush_gpu, fallback, eviction of a
    # queued raster. Skips without Vulkan, compute or ThorVG.
    add_executable(path_texture_cache_gpu_tests
        batched2d/path_texture_cache_gpu_tests.cpp)
    target_link_libraries(path_texture_cache_gpu_tests PRIVATE tessera_gpu tessera_path tessera_rhi_vulkan)
    add_test(NAME path_texture_cache_gpu COMMAND path_texture_cache_gpu_tests)
    set_tests_properties(path_texture_cache_gpu PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # RHI 1.3 sampled render target (#254) pixel test: draw a backdrop, break, copy it into a pooled
    # target, blur it horizontally into a second pooled target that samples the copy, then resume
    # and composite the blurred result. Expected pixels come from the CPU glass kernel.
    add_executable(glass_composite_gpu_tests
        batched2d/glass_composite_gpu_tests.cpp)
    target_link_libraries(glass_composite_gpu_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME glass_composite_gpu COMMAND glass_composite_gpu_tests)
    set_tests_properties(glass_composite_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # Glass composite (#244) pixels through the REAL paint path: paint_scene -> Batched2DSink ->
    # Batched2DRenderer -> Vulkan. A uniform backdrop composites to the analytic colour, a red|blue
    # split is compared pixel by pixel against the CPU Gaussian kernel, the half-resolution rule,
    # the rounded clip, opacity fold and blur(0) pass-through are covered, and the run must be
    # validation-clean.
    add_executable(glass_paint_gpu_tests
        batched2d/glass_paint_gpu_tests.cpp)
    target_link_libraries(glass_paint_gpu_tests PRIVATE
        tessera_gpu tessera_paint tessera_rhi_vulkan)
    add_test(NAME glass_paint_gpu COMMAND glass_paint_gpu_tests)
    set_tests_properties(glass_paint_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # Isolated layers (issue #227 slice 2) on a real device: overlapping children composite as one
    # faded image, the flat fallback differs where it must, premultiplied accumulation, nesting and
    # scissor bounds, validation-clean. Skips without a Vulkan device.
    add_executable(layer_gpu_tests
        batched2d/layer_gpu_tests.cpp)
    target_link_libraries(layer_gpu_tests PRIVATE tessera_gpu tessera_paint tessera_rhi_vulkan)
    add_test(NAME layer_gpu COMMAND layer_gpu_tests)
    set_tests_properties(layer_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # Layer masks (issue #617 slice 2) on a real device: the layer scaled by the mask's alpha, the
    # hole where the mask drew nothing, composition with the group opacity, the unmasked fallback,
    # validation-clean. Skips without a Vulkan device.
    add_executable(layer_mask_gpu_tests
        batched2d/layer_mask_gpu_tests.cpp)
    target_link_libraries(layer_mask_gpu_tests PRIVATE tessera_gpu tessera_paint tessera_rhi_vulkan)
    add_test(NAME layer_mask_gpu COMMAND layer_mask_gpu_tests)
    set_tests_properties(layer_mask_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    # `mask-composite` (issue #617, RHI 1.21) on a real device: subtract, intersect and exclude of a
    # mask layer against the one below it, opaque and half-alpha, the refusal without the pipelines
    # and recycled targets. Skips without a Vulkan device.
    add_executable(layer_mask_composite_gpu_tests
        batched2d/layer_mask_composite_gpu_tests.cpp)
    target_link_libraries(layer_mask_composite_gpu_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME layer_mask_composite_gpu COMMAND layer_mask_composite_gpu_tests)
    set_tests_properties(layer_mask_composite_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    # Retained layers (issue #617, offscreen-layers.md section 5.6) on a real device: a retained frame
    # is byte-identical to the uncached paint across full, damage-clipped and changed frames. Skips
    # without a Vulkan device.
    add_executable(layer_retain_gpu_tests
        batched2d/layer_retain_gpu_tests.cpp)
    target_link_libraries(layer_retain_gpu_tests PRIVATE tessera_gpu tessera_paint tessera_rhi_vulkan)
    add_test(NAME layer_retain_gpu COMMAND layer_retain_gpu_tests)
    set_tests_properties(layer_retain_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    # Frame cost of retained layers, retained vs uncached on the null backend and Vulkan. A tool for the
    # measurements in docs/design/gpu/offscreen-layers-retention.md, not a test.
    add_executable(layer_retain_bench
        batched2d/layer_retain_bench.cpp)
    target_link_libraries(layer_retain_bench PRIVATE tessera_gpu tessera_paint tessera_rhi_null
                          tessera_rhi_vulkan)
    # Per-fragment gradients (ADR-076) on a real device: every kind against the CPU twin, the ramp
    # rules, transforms, clips, one-draw batching, the table limit and frames in flight. Skips
    # without a Vulkan device.
    add_executable(gradient_gpu_tests
        batched2d/gradient_gpu_tests.cpp)
    target_link_libraries(gradient_gpu_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME gradient_gpu COMMAND gradient_gpu_tests)
    set_tests_properties(gradient_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    # The per-fragment gradients against Chromium (ADR-076): fifteen scenes through the paint path,
    # shader and banded fallback side by side. Skips without a Vulkan device.
    add_executable(gradient_chromium_gpu_tests
        batched2d/gradient_chromium_gpu_tests.cpp)
    target_include_directories(gradient_chromium_gpu_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/batched2d)
    target_link_libraries(gradient_chromium_gpu_tests PRIVATE tessera_gpu tessera_paint tessera_rhi_vulkan)
    add_test(NAME gradient_chromium_gpu COMMAND gradient_chromium_gpu_tests)
    set_tests_properties(gradient_chromium_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    # Layer filters (issue #617): blur() and drop-shadow() pixels against the analytic Gaussian,
    # the filter-before-mask order, the paint path, the fallbacks. Skips without a Vulkan device.
    add_executable(layer_filter_gpu_tests
        batched2d/layer_filter_gpu_tests.cpp)
    target_link_libraries(layer_filter_gpu_tests PRIVATE tessera_gpu tessera_paint tessera_rhi_vulkan)
    add_test(NAME layer_filter_gpu COMMAND layer_filter_gpu_tests)
    set_tests_properties(layer_filter_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    # The offscreen layer pool on a real device (issue #617, offscreen-layers.md section 5e): six and
    # twelve sibling masked cards plus blurred cards all mask correctly on two recycled targets, at
    # frame ring depths 1, 2 and 8, validation-clean. Skips without a Vulkan device.
    add_executable(layer_pool_gpu_tests
        batched2d/layer_pool_gpu_tests.cpp)
    target_link_libraries(layer_pool_gpu_tests PRIVATE tessera_gpu tessera_paint tessera_rhi_vulkan)
    add_test(NAME layer_pool_gpu COMMAND layer_pool_gpu_tests)
    set_tests_properties(layer_pool_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # Damage-limited redraw of a filtered node equals a full redraw (the filter dependency walk).
    # Skips without a Vulkan device.
    add_executable(filter_damage_gpu_tests
        batched2d/filter_damage_gpu_tests.cpp)
    target_link_libraries(filter_damage_gpu_tests PRIVATE
        tessera_gpu tessera_paint tessera_frame tessera_layout tessera_runtime tessera_damage
        tessera_golden tessera_rhi_vulkan)
    add_test(NAME filter_damage_gpu COMMAND filter_damage_gpu_tests)
    set_tests_properties(filter_damage_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # The mask-image lavapipe golden (issue #617): the CPU corpus's `masks` scene through the paint
    # path and the renderer's mask step on a software device, against tests/golden/gpu/mask_image.png
    # and against the CPU twin. The binary builds wherever the Vulkan backend does; the ctest entry is
    # registered on the lavapipe lane only (UNIX, a lavapipe manifest present), and the test itself
    # skips on a hardware device.
    add_executable(layer_mask_golden_tests
        batched2d/layer_mask_golden_tests.cpp
        cpu_wide_mask_scenes.cpp)
    target_include_directories(layer_mask_golden_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
    target_compile_definitions(layer_mask_golden_tests PRIVATE
        TESSERA_MASK_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/tests/golden/gpu/mask_image.png")
    target_link_libraries(layer_mask_golden_tests PRIVATE
        tessera_gpu tessera_paint tessera_software tessera_harness_image tessera_rhi_vulkan)
    if (UNIX)
        find_file(TESSERA_MASK_LAVAPIPE_ICD
            NAMES lvp_icd.x86_64.json lvp_icd.json
            PATHS /usr/share/vulkan/icd.d /etc/vulkan/icd.d)
        if (TESSERA_MASK_LAVAPIPE_ICD)
            add_test(NAME layer_mask_golden COMMAND layer_mask_golden_tests)
            set_tests_properties(layer_mask_golden PROPERTIES
                ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_MASK_LAVAPIPE_ICD}"
                SKIP_REGULAR_EXPRESSION "skipped"
                FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
        endif()
    endif()

    # The rounded CPU twin against the renderer, hard and anti-aliased edges (issue #617). Skips
    # without a software Vulkan device.
    add_executable(rounded_twin_gpu_tests
        batched2d/rounded_twin_gpu_tests.cpp
        cpu_wide_rounded_scenes.cpp)
    target_include_directories(rounded_twin_gpu_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
    target_link_libraries(rounded_twin_gpu_tests PRIVATE
        tessera_gpu tessera_paint tessera_software tessera_rhi_vulkan)
    add_test(NAME rounded_twin_gpu COMMAND rounded_twin_gpu_tests)
    set_tests_properties(rounded_twin_gpu PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # Screen-space SDF edges and rounded-clip AA (display-aware-rendering.md S2 and S4).
    add_executable(aa_edge_tests
        batched2d/aa_edge_tests.cpp)
    target_link_libraries(aa_edge_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME aa_edge COMMAND aa_edge_tests)
    set_tests_properties(aa_edge PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(msaa_tests
        batched2d/msaa_tests.cpp)
    target_link_libraries(msaa_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME msaa COMMAND msaa_tests)
    set_tests_properties(msaa PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(paint_text_gpu_tests
        text/paint_text_tests.cpp)
    target_link_libraries(paint_text_gpu_tests PRIVATE
        tessera_paint tessera_text_render tessera_gpu tessera_rhi_vulkan)
    add_test(NAME paint_text_gpu COMMAND paint_text_gpu_tests "${TESSERA_FONT_FIXTURE}")
    set_tests_properties(paint_text_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    add_executable(paint_gradient_gpu_tests
        paint_gradient_tests.cpp)
    target_include_directories(paint_gradient_gpu_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
    target_link_libraries(paint_gradient_gpu_tests PRIVATE
        tessera_paint tessera_text_render tessera_gpu tessera_rhi_vulkan tessera_software)
    add_test(NAME paint_gradient_gpu COMMAND paint_gradient_gpu_tests)
    set_tests_properties(paint_gradient_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(paint_bidi_gpu_tests
        text/paint_bidi_tests.cpp)
    target_link_libraries(paint_bidi_gpu_tests PRIVATE
        tessera_paint tessera_text_render tessera_gpu tessera_rhi_vulkan)
    add_test(NAME paint_bidi_gpu
        COMMAND paint_bidi_gpu_tests "${TESSERA_BIDI_FONT_FIXTURE}")
    set_tests_properties(paint_bidi_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(damage_present_tests
        batched2d/damage_present_tests.cpp)
    target_link_libraries(damage_present_tests PRIVATE
        tessera_gpu tessera_paint tessera_text_render tessera_frame tessera_runtime
        tessera_rhi_vulkan)
    add_test(NAME damage_present
        COMMAND damage_present_tests "${TESSERA_FONT_FIXTURE}")
    set_tests_properties(damage_present PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(damage_min_tests
        batched2d/damage_min_tests.cpp)
    target_link_libraries(damage_min_tests PRIVATE
        tessera_gpu tessera_paint tessera_rhi_vulkan)
    add_test(NAME damage_min COMMAND damage_min_tests)
    set_tests_properties(damage_min PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(vulkan_surface_format_tests
        vulkan/vulkan_surface_format_tests.cpp)
    target_include_directories(vulkan_surface_format_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/src)
    target_link_libraries(vulkan_surface_format_tests PRIVATE tessera_rhi)
    add_test(NAME vulkan_surface_format COMMAND vulkan_surface_format_tests)

    add_executable(vulkan_srgb_tests
        vulkan/vulkan_srgb_tests.cpp)
    target_link_libraries(vulkan_srgb_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME vulkan_srgb COMMAND vulkan_srgb_tests)

    # D7: fault-injection seam + internal device header, no platform surface needed.
    add_executable(vulkan_device_loss_tests
        vulkan/vulkan_device_loss_tests.cpp)
    target_link_libraries(vulkan_device_loss_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    target_include_directories(vulkan_device_loss_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/src
        ${PROJECT_SOURCE_DIR}/third_party/volk
        ${PROJECT_SOURCE_DIR}/third_party/vma)
    target_compile_definitions(vulkan_device_loss_tests PRIVATE VK_NO_PROTOTYPES)
    if (WIN32)
        # Matches tessera_rhi_vulkan; the internal device layout depends on the WSI guards.
        target_compile_definitions(vulkan_device_loss_tests PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    elseif (TESSERA_X11_FOUND)
        target_compile_definitions(vulkan_device_loss_tests PRIVATE VK_USE_PLATFORM_XLIB_KHR)
        target_link_libraries(vulkan_device_loss_tests PRIVATE X11::X11)
    endif()
    add_test(NAME vulkan_device_loss COMMAND vulkan_device_loss_tests)
    # Any "[vulkan] ..." line is a validation warning/error and fails the test.
    set_tests_properties(vulkan_device_loss PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(vulkan_device_recovery_loop_tests
        vulkan/vulkan_device_recovery_loop_tests.cpp)
    target_link_libraries(vulkan_device_recovery_loop_tests PRIVATE
        tessera_gpu tessera_rhi_vulkan tessera_runtime)
    target_include_directories(vulkan_device_recovery_loop_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/src
        ${PROJECT_SOURCE_DIR}/third_party/volk
        ${PROJECT_SOURCE_DIR}/third_party/vma)
    target_compile_definitions(vulkan_device_recovery_loop_tests PRIVATE VK_NO_PROTOTYPES)
    if (WIN32)
        target_compile_definitions(vulkan_device_recovery_loop_tests PRIVATE
            VK_USE_PLATFORM_WIN32_KHR)
    elseif (TESSERA_X11_FOUND)
        target_compile_definitions(vulkan_device_recovery_loop_tests PRIVATE
            VK_USE_PLATFORM_XLIB_KHR)
        target_link_libraries(vulkan_device_recovery_loop_tests PRIVATE X11::X11)
    endif()
    add_test(NAME vulkan_device_recovery_loop COMMAND vulkan_device_recovery_loop_tests)
    set_tests_properties(vulkan_device_recovery_loop PROPERTIES
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(present_hardening_tests
        vulkan/present_hardening_tests.cpp)
    target_link_libraries(present_hardening_tests PRIVATE
        tessera_gpu tessera_text_render tessera_rhi_vulkan tessera_runtime)
    target_include_directories(present_hardening_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/src
        ${PROJECT_SOURCE_DIR}/third_party/volk
        ${PROJECT_SOURCE_DIR}/third_party/vma)
    target_compile_definitions(present_hardening_tests PRIVATE VK_NO_PROTOTYPES)
    if (WIN32)
        target_compile_definitions(present_hardening_tests PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    elseif (TESSERA_X11_FOUND)
        target_compile_definitions(present_hardening_tests PRIVATE VK_USE_PLATFORM_XLIB_KHR)
        target_link_libraries(present_hardening_tests PRIVATE X11::X11)
    endif()
    add_test(NAME present_hardening
        COMMAND present_hardening_tests "${TESSERA_FONT_FIXTURE}")
    set_tests_properties(present_hardening PROPERTIES
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(vulkan_texture_churn_tests
        vulkan/vulkan_texture_churn_tests.cpp)
    target_link_libraries(vulkan_texture_churn_tests PRIVATE tessera_rhi_vulkan)
    add_test(NAME vulkan_texture_churn COMMAND vulkan_texture_churn_tests)

    add_executable(vulkan_descriptor_pool_growth_tests
        vulkan/vulkan_descriptor_pool_growth_tests.cpp)
    target_link_libraries(vulkan_descriptor_pool_growth_tests PRIVATE tessera_rhi_vulkan)
    add_test(NAME vulkan_descriptor_pool_growth COMMAND vulkan_descriptor_pool_growth_tests)

    add_executable(vulkan_device_selection_tests
        vulkan/vulkan_device_selection_tests.cpp)
    target_include_directories(vulkan_device_selection_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/src)
    add_test(NAME vulkan_device_selection COMMAND vulkan_device_selection_tests)

    add_executable(vulkan_create_failure_tests
        vulkan/vulkan_create_failure_tests.cpp)
    target_link_libraries(vulkan_create_failure_tests PRIVATE tessera_rhi_vulkan)
    add_test(NAME vulkan_create_failure COMMAND vulkan_create_failure_tests)

    add_executable(vulkan_async_upload_tests
        vulkan/vulkan_async_upload_tests.cpp)
    target_link_libraries(vulkan_async_upload_tests PRIVATE tessera_rhi_vulkan)
    add_test(NAME vulkan_async_upload COMMAND vulkan_async_upload_tests)
    set_tests_properties(vulkan_async_upload PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # RHI 1.16 compute (issue #683): a dispatch, a barrier and a readback copy on a real device. The
    # probe shader is compiled by this rule alone, so it needs no shader-manifest entry.
    if (Vulkan_GLSLC_EXECUTABLE)
        set(TESSERA_COMPUTE_PROBE_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/compute_probe_comp_spv.h)
        add_custom_command(
            OUTPUT ${TESSERA_COMPUTE_PROBE_HEADER}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} -mfmt=c --target-env=vulkan1.3
                    ${CMAKE_CURRENT_SOURCE_DIR}/rhi/compute_probe.comp -o ${TESSERA_COMPUTE_PROBE_HEADER}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rhi/compute_probe.comp
            COMMENT "glslc compute_probe.comp"
            VERBATIM)
        add_executable(compute_dispatch_tests
            rhi/compute_dispatch_tests.cpp ${TESSERA_COMPUTE_PROBE_HEADER})
        target_include_directories(compute_dispatch_tests PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated)
        target_link_libraries(compute_dispatch_tests PRIVATE tessera_rhi_vulkan tessera_rhi_null)
        add_test(NAME compute_dispatch COMMAND compute_dispatch_tests)
        set_tests_properties(compute_dispatch PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

        # RHI 1.18 conformance kernels (issue #684): indirect dispatch, buffer atomics, a shared-memory
        # prefix sum, with exact expected output. Same rule as the probe: no manifest entry.
        set(TESSERA_COMPUTE_CONFORMANCE_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/compute_conformance_comp_spv.h)
        add_custom_command(
            OUTPUT ${TESSERA_COMPUTE_CONFORMANCE_HEADER}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
            COMMAND ${Vulkan_GLSLC_EXECUTABLE} -mfmt=c --target-env=vulkan1.3
                    ${CMAKE_CURRENT_SOURCE_DIR}/rhi/compute_conformance.comp
                    -o ${TESSERA_COMPUTE_CONFORMANCE_HEADER}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/rhi/compute_conformance.comp
            COMMENT "glslc compute_conformance.comp"
            VERBATIM)
        add_executable(compute_conformance_tests
            rhi/compute_conformance_tests.cpp ${TESSERA_COMPUTE_CONFORMANCE_HEADER})
        target_include_directories(compute_conformance_tests PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated)
        target_link_libraries(compute_conformance_tests PRIVATE tessera_rhi_vulkan tessera_rhi_null)
        add_test(NAME compute_conformance COMMAND compute_conformance_tests)
        set_tests_properties(compute_conformance PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    endif()

    add_executable(vulkan_async_submit_tests
        vulkan/vulkan_async_submit_tests.cpp)
    target_link_libraries(vulkan_async_submit_tests PRIVATE tessera_rhi_vulkan)
    add_test(NAME vulkan_async_submit COMMAND vulkan_async_submit_tests)
    # The device prints validation warnings/errors as "[vulkan] ..."; any of them is a failure.
    set_tests_properties(vulkan_async_submit PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
endif()

if (WIN32 AND TARGET tessera_rhi_vulkan)
    add_executable(vulkan_swapchain_tests
        vulkan/vulkan_swapchain_tests.cpp)

    target_compile_definitions(vulkan_swapchain_tests PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    target_include_directories(vulkan_swapchain_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/src
        ${PROJECT_SOURCE_DIR}/third_party/volk
        ${PROJECT_SOURCE_DIR}/third_party/vma)
    target_link_libraries(vulkan_swapchain_tests PRIVATE
        tessera_gpu
        tessera_rhi_vulkan
        tessera_hal_window)

    add_test(NAME vulkan_swapchain COMMAND vulkan_swapchain_tests)
    set_tests_properties(vulkan_swapchain PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(vulkan_swapchain_async_tests
        vulkan/vulkan_swapchain_async_tests.cpp)
    target_link_libraries(vulkan_swapchain_async_tests PRIVATE
        tessera_gpu
        tessera_rhi_vulkan
        tessera_hal_window)
    add_test(NAME vulkan_swapchain_async COMMAND vulkan_swapchain_async_tests)
    set_tests_properties(vulkan_swapchain_async PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
endif()

if (TESSERA_X11_FOUND AND TARGET tessera_rhi_vulkan AND TARGET tessera_hal_window)
    add_executable(vulkan_swapchain_x11_tests
        vulkan/vulkan_swapchain_x11_tests.cpp)

    target_compile_definitions(vulkan_swapchain_x11_tests PRIVATE VK_USE_PLATFORM_XLIB_KHR)
    target_include_directories(vulkan_swapchain_x11_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/src
        ${PROJECT_SOURCE_DIR}/third_party/volk
        ${PROJECT_SOURCE_DIR}/third_party/vma)
    target_link_libraries(vulkan_swapchain_x11_tests PRIVATE
        tessera_gpu
        tessera_rhi_vulkan
        tessera_hal_window
        X11::X11)

    add_test(NAME vulkan_swapchain_x11 COMMAND vulkan_swapchain_x11_tests)
    set_tests_properties(vulkan_swapchain_x11 PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
endif()

# Wayland presentation smoke (issue #155): creates the window, a Vulkan device bound to the Wayland
# surface family, a swapchain on the wl_surface, presents one frame and tears down. Skips with a
# diagnostic where there is no compositor (WAYLAND_DISPLAY unset/unreachable), no Vulkan device or
# no VK_KHR_wayland_surface in the driver; verified headless under weston + lavapipe.
if (TESSERA_WAYLAND_FOUND AND TARGET tessera_rhi_vulkan AND TARGET tessera_hal_window)
    add_executable(vulkan_swapchain_wayland_tests
        vulkan/vulkan_swapchain_wayland_tests.cpp)

    target_compile_definitions(vulkan_swapchain_wayland_tests PRIVATE VK_USE_PLATFORM_WAYLAND_KHR)
    target_include_directories(vulkan_swapchain_wayland_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/src
        ${PROJECT_SOURCE_DIR}/third_party/volk
        ${PROJECT_SOURCE_DIR}/third_party/vma
        ${TESSERA_WAYLAND_CLIENT_INCLUDE_DIRS})
    target_link_libraries(vulkan_swapchain_wayland_tests PRIVATE
        tessera_gpu
        tessera_rhi_vulkan
        tessera_hal_window
        ${TESSERA_WAYLAND_CLIENT_LIBRARIES})
    target_link_directories(vulkan_swapchain_wayland_tests PRIVATE
        ${TESSERA_WAYLAND_CLIENT_LIBRARY_DIRS})

    add_test(NAME vulkan_swapchain_wayland COMMAND vulkan_swapchain_wayland_tests)
    set_tests_properties(vulkan_swapchain_wayland PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
endif()

# App-window driver: windowless capture parity and the hidden-window present smoke. The library is
# host-only (Vulkan + platform window); the tests skip where those are unavailable.
if (TARGET tessera_app_window)
    add_executable(app_window_capture_tests
        app_window_capture_tests.cpp)
    target_compile_definitions(app_window_capture_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_capture_tests PRIVATE tessera_app_window)
    add_test(NAME app_window_capture COMMAND app_window_capture_tests)
    set_tests_properties(app_window_capture PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")

    # A filtered card (issue #617) presented through a real swapchain: the direct path reopens the
    # swapchain image with a Load pass for its layer passes. Prints `skipped` without a presentation
    # surface, which the property below turns into a skip.
    add_executable(app_window_filter_tests
        app_window_filter_tests.cpp)
    target_compile_definitions(app_window_filter_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_filter_tests PRIVATE tessera_app_window)
    add_test(NAME app_window_filter COMMAND app_window_filter_tests)
    set_tests_properties(app_window_filter PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    add_executable(app_window_smoke_tests
        app_window_smoke_tests.cpp)
    target_compile_definitions(app_window_smoke_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_smoke_tests PRIVATE tessera_app_window)
    add_test(NAME app_window_smoke COMMAND app_window_smoke_tests)
    set_tests_properties(app_window_smoke PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")

    # Multi-window (issue #228, docs/design/multi-window.md): two windows present through one shared
    # `WindowDevice`, closing one leaves the other running, and a resize of one does not relayout the
    # other. It runs on whatever device the lane has; a lane with no presentation surface prints
    # `skipped`, which the property below turns into a skip rather than a pass.
    # The offscreen pool on a real window (offscreen-layers.md section 5e): six masked boxes need two
    # live targets, and a pool too small for them is a counter, run stats and one warning.
    add_executable(app_window_offscreen_tests
        app_window_offscreen_tests.cpp)
    target_compile_definitions(app_window_offscreen_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_offscreen_tests PRIVATE tessera_app_window tessera_diagnostics)
    add_test(NAME app_window_offscreen COMMAND app_window_offscreen_tests)
    set_tests_properties(app_window_offscreen PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")

    add_executable(app_window_multi_tests
        app_window_multi_tests.cpp)
    target_compile_definitions(app_window_multi_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_multi_tests PRIVATE tessera_app_window)
    add_test(NAME app_window_multi COMMAND app_window_multi_tests)
    set_tests_properties(app_window_multi PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")

    # Issue #180: two windows driven by ONE loop. `WindowDevice::pump_windows_once()` gives every
    # registered window one bounded iteration, so the acceptance's "independent lifecycle, damage and
    # pacing" is asserted on the real GPU: both windows present in the same group tick, a change in
    # one paints only that one, and a settled window presents nothing. A lane with no presentation
    # surface prints `skipped`, which the property below turns into a skip rather than a pass.
    add_executable(app_window_multi_loop_tests
        app_window_multi_loop_tests.cpp)
    target_compile_definitions(app_window_multi_loop_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_multi_loop_tests PRIVATE tessera_app_window)
    add_test(NAME app_window_multi_loop COMMAND app_window_multi_loop_tests)
    set_tests_properties(app_window_multi_loop PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")

    # Issue #181: a tray app runs with no visible window and zero presents, and shows a window on
    # activation. The acceptance is asserted on the real GPU because "zero presents" is a claim about
    # the present path: a lane with no presentation surface prints `skipped`, which the property
    # below turns into a skip rather than a pass.
    add_executable(app_window_tray_tests
        app_window_tray_tests.cpp)
    target_compile_definitions(app_window_tray_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_tray_tests PRIVATE tessera_app_window)
    add_test(NAME app_window_tray COMMAND app_window_tray_tests)
    set_tests_properties(app_window_tray PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")

    # Multi-frame sync: two frames in flight paced by one timeline semaphore on the swapchain path,
    # driven through the single-iteration seam, with the backend's validation counter proving the
    # run (including teardown) is validation-clean.
    add_executable(vulkan_timeline_frame_tests
        vulkan_timeline_frame_tests.cpp)
    target_compile_definitions(vulkan_timeline_frame_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(vulkan_timeline_frame_tests PRIVATE tessera_app_window)
    add_test(NAME vulkan_timeline_frames COMMAND vulkan_timeline_frame_tests)
    set_tests_properties(vulkan_timeline_frames PROPERTIES
        # ": skipped" only: the success line reports a slot_skips counter, which a bare "skipped"
        # pattern would misread as a skipped test.
        SKIP_REGULAR_EXPRESSION ": skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # Occluded/minimized pause: the single-iteration seam (WindowHost::pump_once) with metrics
    # snapshots that report the lifecycle, so the pause, the input-draining park and the full
    # repaint on resume are deterministic and need no real OS focus/occlusion change.
    add_executable(app_window_occluded_tests
        app_window_occluded_tests.cpp)
    target_compile_definitions(app_window_occluded_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_occluded_tests PRIVATE tessera_app_window)
    add_test(NAME app_window_occluded COMMAND app_window_occluded_tests)
    set_tests_properties(app_window_occluded PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")

    # Runtime-config pause wiring: the same hidden-window pump_once seam driven twice, first with
    # the default `pauseWhenOccluded=true` (nothing presents while occluded) and then with
    # `WindowOptions::runtime.pauseWhenOccluded=false` (the occluded/minimized window keeps
    # presenting). The disabled-policy arm is also covered at the unit level without a GPU.
    add_executable(app_window_occluded_config_tests
        app_window_occluded_config_tests.cpp)
    target_compile_definitions(app_window_occluded_config_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_occluded_config_tests PRIVATE tessera_app_window)
    add_test(NAME app_window_occluded_config COMMAND app_window_occluded_config_tests)
    set_tests_properties(app_window_occluded_config PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")

    # Host-owned caret blink: a ManualClock injected through WindowOptions::clock drives the blink
    # on a real hidden window through the single-iteration seam. One frame per visibility flip, no
    # frames while the caret is inactive or the window is paused.
    add_executable(app_window_caret_tests
        app_window_caret_tests.cpp)
    target_compile_definitions(app_window_caret_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_caret_tests PRIVATE tessera_app_window)
    add_test(NAME caret_blink_host COMMAND app_window_caret_tests)
    set_tests_properties(caret_blink_host PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")

    # The blink reaching the paint payload (text-editing-v1 section 12): `WindowOptions::caret_active`
    # is the app's focus policy and `WindowOptions::caret_target` is where the driver pushes the blink
    # result. Without both the host's state machine runs but nothing it decides reaches
    # `TextDecoration`, so paint draws a solid caret forever.
    add_executable(app_window_caret_target_tests
        app_window_caret_target_tests.cpp)
    target_compile_definitions(app_window_caret_target_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_caret_target_tests PRIVATE tessera_app_window)
    add_test(NAME caret_blink_target COMMAND app_window_caret_target_tests)
    set_tests_properties(caret_blink_target PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")

    # The adaptive render-scale consumer (docs/design/performance.md 6.1): a real hidden window and
    # swapchain prove that scale 1.0 keeps the direct full-size path, a forced downgrade renders into
    # the smaller target and presents through the resample without recreating the swapchain, and the
    # recovery returns the target extent - all validation-clean.
    add_executable(app_window_render_scale_tests
        app_window_render_scale_tests.cpp)
    target_compile_definitions(app_window_render_scale_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_render_scale_tests PRIVATE
        tessera_app_window
        tessera_scale)
    add_test(NAME app_window_render_scale COMMAND app_window_render_scale_tests)

    # The SVG stack on the real window path (ADR-071): an asset bundle given through WindowOptions is
    # painted by a scene node, every frame a new raster that routes to the GPU rasterizer, flushed
    # ahead of the draw list - validation-clean with two frames in flight.
    add_executable(app_window_vector_tests
        app_window_vector_tests.cpp)
    target_compile_definitions(app_window_vector_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_vector_tests PRIVATE
        tessera_app_window
        tessera_path)
    add_test(NAME app_window_vector COMMAND app_window_vector_tests)
    set_tests_properties(app_window_vector PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    set_tests_properties(app_window_render_scale PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # The adaptive present cap on the real window path (docs/design/performance.md section 6, step
    # 3): a downgrade lowers the pacer's cap to the next refresh divisor, so the effective present
    # interval doubles at the next frame boundary and returns on upgrade - through the pacer's
    # existing rate-change path, with no swapchain recreation.
    add_executable(app_window_present_cap_tests
        app_window_present_cap_tests.cpp)
    target_compile_definitions(app_window_present_cap_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_present_cap_tests PRIVATE
        tessera_app_window)
    add_test(NAME app_window_present_cap COMMAND app_window_present_cap_tests)
    set_tests_properties(app_window_present_cap PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped"
        FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

    # Window-path input: real hidden window + swapchain, normalized events posted through
    # WindowHost::post_input, generated click/typing handlers. The generated demo is host-only.
    if (TARGET tessera_compiler_demo)
        add_executable(app_window_input_tests
            app_window_input_tests.cpp)
        target_compile_definitions(app_window_input_tests PRIVATE
            TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
        target_link_libraries(app_window_input_tests PRIVATE
            tessera_app_window
            tessera_compiler_demo)
        add_test(NAME app_window_input COMMAND app_window_input_tests)
        set_tests_properties(app_window_input PROPERTIES
            SKIP_REGULAR_EXPRESSION "skipped")
    endif()

    # Device-loss recovery on the real window path: one injected device-lost fault, release of the
    # lost generation, recovery through the runtime controller, a recreated device + presentation
    # stack, and frames presented afterwards. Needs the backend fault-injection seam.
    if (TESSERA_VULKAN_TEST_HOOKS)
        add_executable(app_window_recovery_tests
            app_window_recovery_tests.cpp)
        target_include_directories(app_window_recovery_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/src
            ${PROJECT_SOURCE_DIR}/third_party/volk
            ${PROJECT_SOURCE_DIR}/third_party/vma)
        target_compile_definitions(app_window_recovery_tests PRIVATE
            VK_NO_PROTOTYPES
            TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
        if (WIN32)
            target_compile_definitions(app_window_recovery_tests PRIVATE
                VK_USE_PLATFORM_WIN32_KHR)
        elseif (TESSERA_X11_FOUND)
            target_compile_definitions(app_window_recovery_tests PRIVATE
                VK_USE_PLATFORM_XLIB_KHR)
            target_link_libraries(app_window_recovery_tests PRIVATE X11::X11)
        endif()
        target_link_libraries(app_window_recovery_tests PRIVATE
            tessera_app_window
            tessera_rhi_vulkan)
        add_test(NAME app_window_recovery COMMAND app_window_recovery_tests)
        set_tests_properties(app_window_recovery PROPERTIES
            SKIP_REGULAR_EXPRESSION "skipped"
            FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    endif()
endif()


# Harness golden/gpu-smoke frame capture: capability gating (no device) plus an offscreen render of
# the fixture scene and a comparison against itself. Skips where Vulkan or lavapipe is unavailable.
if (TARGET tessera_harness_capture)
    add_executable(harness_frame_capture_tests
        harness_frame_capture_tests.cpp)
    target_compile_definitions(harness_frame_capture_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf"
        TESSERA_HARNESS_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/baseline/harness")
    target_link_libraries(harness_frame_capture_tests PRIVATE tessera_harness_capture)
    add_test(NAME harness_frame_capture COMMAND harness_frame_capture_tests)
    set_tests_properties(harness_frame_capture PROPERTIES
        SKIP_REGULAR_EXPRESSION "skipped")
endif()
