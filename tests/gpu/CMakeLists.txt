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

add_executable(batched2d_tests
    batched2d/batched2d_tests.cpp)
target_link_libraries(batched2d_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME batched2d COMMAND batched2d_tests)

add_executable(frames_in_flight_tests
    batched2d/frames_in_flight_tests.cpp)
target_link_libraries(frames_in_flight_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME frames_in_flight COMMAND frames_in_flight_tests)

add_executable(damage_redraw_tests
    batched2d/damage_redraw_tests.cpp)
target_link_libraries(damage_redraw_tests PRIVATE tessera_gpu tessera_rhi_null tessera_damage)
add_test(NAME damage_redraw COMMAND damage_redraw_tests)

set(TESSERA_FONT_FIXTURE
    "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/fonts/NotoSans-Regular.ttf")
get_filename_component(TESSERA_FONT_FIXTURE "${TESSERA_FONT_FIXTURE}" REALPATH)

add_executable(text_render_tests
    text/text_render_tests.cpp)
target_link_libraries(text_render_tests PRIVATE tessera_text_render tessera_rhi_null)
add_test(NAME text_render COMMAND text_render_tests "${TESSERA_FONT_FIXTURE}")

# Path raster cache -> batched2d textured draw. The null-device section runs everywhere; the Vulkan
# readback cases are compiled in only when the Vulkan backend is part of the build.
add_executable(path_texture_draw_tests
    batched2d/path_texture_draw_tests.cpp)
target_link_libraries(path_texture_draw_tests PRIVATE
    tessera_gpu tessera_path tessera_rhi_null)
if (TARGET tessera_rhi_vulkan)
    target_link_libraries(path_texture_draw_tests PRIVATE tessera_rhi_vulkan)
    target_compile_definitions(path_texture_draw_tests PRIVATE TESSERA_TEST_HAS_VULKAN)
endif()
add_test(NAME path_texture_draw COMMAND path_texture_draw_tests)
set_tests_properties(path_texture_draw PROPERTIES FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

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
        COMMAND batched2d_poc --backend vulkan --on-demand --width 320 --height 240)
    set_tests_properties(batched2d_on_demand PROPERTIES
        PASS_REGULAR_EXPRESSION "on-demand: rendered=([3-9]|[1-9][0-9]+) presented=[0-9]+ scripted=2 events=[0-9]+ idle_waits=[1-9][0-9]* unjustified=0 missed=0|skipped")
endif()

if (TARGET batched2d_poc)
    add_test(
        NAME batched2d_cpu_golden
        COMMAND batched2d_poc --backend cpu --tolerance 0 --max-mismatch 0
            --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_cpu_golden.png
            --compare ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/dashboard_cpu.png)
endif()

if (UNIX AND TARGET batched2d_poc)
    find_file(TESSERA_LAVAPIPE_ICD
        NAMES lvp_icd.x86_64.json lvp_icd.json
        PATHS /usr/share/vulkan/icd.d /etc/vulkan/icd.d)

    if (TESSERA_LAVAPIPE_ICD)
        add_test(
            NAME batched2d_golden
            COMMAND batched2d_poc --backend vulkan --golden --tolerance 0 --max-mismatch 0
                --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_golden.png
                --compare ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/dashboard.png)
        set_tests_properties(batched2d_golden PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}")

        add_test(
            NAME batched2d_overlay_golden
            COMMAND batched2d_poc --backend vulkan --golden --overlay
                --font ${TESSERA_FONT_FIXTURE} --tolerance 1 --max-mismatch 0.001
                --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_overlay_golden.png
                --compare ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/dashboard_overlay.png)
        set_tests_properties(batched2d_overlay_golden PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}")

        add_test(
            NAME batched2d_text_golden
            COMMAND batched2d_poc --backend vulkan --golden
                --font ${TESSERA_FONT_FIXTURE} --tolerance 1 --max-mismatch 0.001
                --out ${CMAKE_CURRENT_BINARY_DIR}/batched2d_text_golden.png
                --compare ${CMAKE_CURRENT_SOURCE_DIR}/../golden/baseline/batched2d/dashboard_text.png)
        set_tests_properties(batched2d_text_golden PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}")

        add_test(
            NAME batched2d_shapes_golden
            COMMAND batched2d_poc --backend vulkan --golden --shapes --tolerance 0 --max-mismatch 0
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
            COMMAND batched2d_poc --backend vulkan --on-demand --width 320 --height 240)
        set_tests_properties(batched2d_on_demand PROPERTIES
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}"
            PASS_REGULAR_EXPRESSION "on-demand: rendered=([3-9]|[1-9][0-9]+) presented=[0-9]+ scripted=2 events=[0-9]+ idle_waits=[1-9][0-9]* unjustified=0 missed=0|skipped")
    endif()
endif()

if (TARGET tessera_rhi_vulkan)
    add_executable(damage_equivalence_tests
        batched2d/damage_equivalence_tests.cpp)
    target_link_libraries(damage_equivalence_tests PRIVATE
        tessera_gpu tessera_damage tessera_golden tessera_rhi_vulkan)
    add_test(NAME damage_equivalence COMMAND damage_equivalence_tests)

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
