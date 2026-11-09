# SPDX-License-Identifier: MIT

add_executable(rhi_null_tests
    rhi/rhi_null_tests.cpp)
target_link_libraries(rhi_null_tests PRIVATE tessera_rhi_null)
add_test(NAME rhi_null COMMAND rhi_null_tests)

add_executable(batched2d_tests
    batched2d/batched2d_tests.cpp)
target_link_libraries(batched2d_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME batched2d COMMAND batched2d_tests)

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
            ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}")
    endif()
endif()

if (TARGET tessera_rhi_vulkan)
    add_executable(damage_equivalence_tests
        batched2d/damage_equivalence_tests.cpp)
    target_link_libraries(damage_equivalence_tests PRIVATE
        tessera_gpu tessera_damage tessera_golden tessera_rhi_vulkan)
    add_test(NAME damage_equivalence COMMAND damage_equivalence_tests)

    add_executable(vulkan_texture_churn_tests
        vulkan/vulkan_texture_churn_tests.cpp)
    target_link_libraries(vulkan_texture_churn_tests PRIVATE tessera_rhi_vulkan)
    add_test(NAME vulkan_texture_churn COMMAND vulkan_texture_churn_tests)

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
endif()
