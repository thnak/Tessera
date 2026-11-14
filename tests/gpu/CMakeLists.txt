# SPDX-License-Identifier: MIT

add_executable(rhi_null_tests
    rhi/rhi_null_tests.cpp)
target_link_libraries(rhi_null_tests PRIVATE tessera_rhi_null)
add_test(NAME rhi_null COMMAND rhi_null_tests)

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

add_executable(damage_redraw_tests
    batched2d/damage_redraw_tests.cpp)
target_link_libraries(damage_redraw_tests PRIVATE tessera_gpu tessera_rhi_null tessera_damage)
add_test(NAME damage_redraw COMMAND damage_redraw_tests)

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
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${TESSERA_BATCHED2D_GLSL_DIR}/batched2d.vert"
    "${TESSERA_BATCHED2D_GLSL_DIR}/batched2d.frag")
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

# Manual measurement tool (not a CTest): path upload / cache-hit / SVG decode cost on a real device.
if (TARGET tessera_rhi_vulkan)
    add_executable(path_texture_measure
        batched2d/path_texture_measure.cpp)
    target_link_libraries(path_texture_measure PRIVATE
        tessera_gpu tessera_path tessera_rhi_null tessera_rhi_vulkan)
    target_compile_definitions(path_texture_measure PRIVATE
        TESSERA_SVG_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/svg")
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
    cpu_wide_golden_tests.cpp)
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

    # Isolated-layer seam (#242) pixel test: draw, break the pass, copy a region, resume, draw.
    add_executable(renderer_seam_gpu_tests
        batched2d/renderer_seam_gpu_tests.cpp)
    target_link_libraries(renderer_seam_gpu_tests PRIVATE tessera_gpu tessera_rhi_vulkan)
    add_test(NAME renderer_seam_gpu COMMAND renderer_seam_gpu_tests)
    set_tests_properties(renderer_seam_gpu PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

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

    add_executable(app_window_smoke_tests
        app_window_smoke_tests.cpp)
    target_compile_definitions(app_window_smoke_tests PRIVATE
        TESSERA_TEST_FONT="${PROJECT_SOURCE_DIR}/tests/fixtures/fonts/NotoSans-Regular.ttf")
    target_link_libraries(app_window_smoke_tests PRIVATE tessera_app_window)
    add_test(NAME app_window_smoke COMMAND app_window_smoke_tests)
    set_tests_properties(app_window_smoke PROPERTIES
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
