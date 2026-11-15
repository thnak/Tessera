# SPDX-License-Identifier: MIT
#
# AF6 (issue #178, ADR-050 section 7): native surfaces and the renderer that composites into one.
#
# Two entries, deliberately split by what they can prove:
#
#  * `native_surface_renderer` - null backend, every lane: the command stream of a composited surface
#    frame plus the states the renderer reports instead of throwing. No GPU, no window.
#  * `native_surface_golden` - the pixel golden of the same composite. The binary builds wherever the
#    Vulkan backend does, but its ctest entry is registered only on the lavapipe lane (UNIX, a
#    lavapipe manifest present, exactly like the `batched2d_*_golden` entries). The test itself also
#    refuses a non-software device by reading `VkPhysicalDeviceProperties` (ADR-009), so a run on a
#    hardware host can never write or verify these pixels even if it is invoked by hand.

# The ownership proof (issue #178's acceptance: "the surface never owns window handles"). Its target
# is built in `src/hal/surface/CMakeLists.txt`; the `add_test` is here because `src/hal/window`, the
# directory that registers that module, is not part of the CTest subdirectory tree - a test
# registered there would build and never run. This directory is in the tree.
add_test(NAME native_surface COMMAND native_surface_tests)

add_executable(native_surface_renderer_tests
    native_surface_renderer_tests.cpp)
target_link_libraries(native_surface_renderer_tests PRIVATE
    tessera_gpu
    tessera_hal_surface
    tessera_rhi_null)
add_test(NAME native_surface_renderer COMMAND native_surface_renderer_tests)

if (TARGET tessera_rhi_vulkan)
    add_executable(native_surface_golden_tests
        native_surface_golden_tests.cpp)
    target_compile_definitions(native_surface_golden_tests PRIVATE
        TESSERA_SURFACE_GOLDEN_PATH="${PROJECT_SOURCE_DIR}/tests/golden/baseline/surface/native_surface.png")
    target_link_libraries(native_surface_golden_tests PRIVATE
        tessera_gpu
        tessera_hal_surface
        tessera_harness_image
        tessera_rhi_vulkan)

    if (UNIX)
        find_file(TESSERA_SURFACE_LAVAPIPE_ICD
            NAMES lvp_icd.x86_64.json lvp_icd.json
            PATHS /usr/share/vulkan/icd.d /etc/vulkan/icd.d)
        if (TESSERA_SURFACE_LAVAPIPE_ICD)
            add_test(NAME native_surface_golden COMMAND native_surface_golden_tests)
            set_tests_properties(native_surface_golden PROPERTIES
                ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_SURFACE_LAVAPIPE_ICD}"
                SKIP_REGULAR_EXPRESSION "skipped"
                FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
        endif()
    endif()
endif()
