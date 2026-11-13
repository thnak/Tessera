# SPDX-License-Identifier: MIT
#
# Cross-backend image parity harness (issue #245). The scene set in parity_scenes.cpp is rendered
# through the RHI on every available arm; lavapipe is the reference (ADR-009) and the per-pair
# tolerances come from tests/golden/tolerance.json (schema 2) as a generated C++ table, so a typo
# fails the configure step instead of silently loosening a comparison.
#
# Registered here so the shared tests/gpu/CMakeLists.txt only needs one add_subdirectory(backends)
# line:
#   gpu_backend_parity_reference - render the lavapipe reference images (skips without lavapipe)
#   gpu_backend_parity           - render every arm, compare against the reference, write artifacts
#   perceptual_metric            - SSIM/deltaE hand-computed unit tests (no GPU)
#   difference_class             - synthetic-image edge-rounding vs structural unit tests (no GPU)
#   parity_policy                - pair tolerance, verdict, report and diff-PNG unit tests (no GPU)

if (NOT Python3_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
set(TESSERA_PARITY_TOLERANCE_FILE "${PROJECT_SOURCE_DIR}/tests/golden/tolerance.json")
set(TESSERA_PARITY_TOLERANCE_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/golden_tolerances.py")
set(TESSERA_PARITY_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${TESSERA_PARITY_GENERATED_DIR}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -B "${TESSERA_PARITY_TOLERANCE_SCRIPT}"
        --name gpu_backend_parity
        --pairs-header "${TESSERA_PARITY_GENERATED_DIR}/parity_tolerances.hpp"
    OUTPUT_VARIABLE parity_header_output
    ERROR_VARIABLE parity_header_error
    RESULT_VARIABLE parity_header_result)
if (NOT parity_header_result EQUAL 0)
    message(FATAL_ERROR
        "tolerance.json: cannot generate the parity pair table: "
        "${parity_header_error}${parity_header_output}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${TESSERA_PARITY_TOLERANCE_FILE}"
    "${TESSERA_PARITY_TOLERANCE_SCRIPT}")

add_library(tessera_gpu_parity STATIC
    parity_backend.cpp
    parity_policy.cpp
    parity_render.cpp
    parity_report.cpp
    parity_scenes.cpp)
target_include_directories(tessera_gpu_parity PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TESSERA_PARITY_GENERATED_DIR})
target_link_libraries(tessera_gpu_parity PUBLIC
    tessera_golden
    tessera_gpu
    tessera_harness_image
    tessera_rhi
    tessera_rhi_null)
target_compile_features(tessera_gpu_parity PUBLIC cxx_std_23)

if (TARGET tessera_rhi_vulkan)
    target_link_libraries(tessera_gpu_parity PUBLIC tessera_rhi_vulkan)
    target_compile_definitions(tessera_gpu_parity PUBLIC TESSERA_TEST_HAS_VULKAN)
endif()
if (TARGET tessera_webgpu)
    target_compile_definitions(tessera_gpu_parity PUBLIC TESSERA_TEST_HAS_WEBGPU)
endif()

add_executable(gpu_backend_parity
    parity_main.cpp)
target_link_libraries(gpu_backend_parity PRIVATE tessera_gpu_parity)

# The lavapipe reference arm: the process must select the software ICD, which CMake wires here when
# it can find the manifest. Without lavapipe the executable prints a skip line and exits 0; the
# ctest entry is reported as skipped rather than silently green.
if (DEFINED ENV{VK_ICD_FILENAMES} AND "$ENV{VK_ICD_FILENAMES}" MATCHES "lvp_icd|lavapipe")
    set(TESSERA_LAVAPIPE_ICD "$ENV{VK_ICD_FILENAMES}")
else()
    find_file(TESSERA_LAVAPIPE_ICD
        NAMES lvp_icd.x86_64.json lvp_icd.aarch64.json lvp_icd.json
        PATHS
            "$ENV{VULKAN_SDK}/etc/vulkan/icd.d"
            "$ENV{VULKAN_SDK}/Bin"
            "$ENV{VULKAN_SDK}"
            /usr/share/vulkan/icd.d
            /etc/vulkan/icd.d
            "$ENV{LOCALAPPDATA}/vulkan/icd.d"
            "C:/Windows/System32")
endif()

set(TESSERA_PARITY_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/parity")
add_test(NAME gpu_backend_parity_reference
    COMMAND gpu_backend_parity --write-reference --out-dir "${TESSERA_PARITY_OUT_DIR}")
if (TESSERA_LAVAPIPE_ICD)
    set_tests_properties(gpu_backend_parity_reference PROPERTIES
        ENVIRONMENT "VK_ICD_FILENAMES=${TESSERA_LAVAPIPE_ICD}")
endif()
set_tests_properties(gpu_backend_parity_reference PROPERTIES
    SKIP_REGULAR_EXPRESSION "gpu_backend_parity: reference skipped")

add_test(NAME gpu_backend_parity
    COMMAND gpu_backend_parity --out-dir "${TESSERA_PARITY_OUT_DIR}")
set_tests_properties(gpu_backend_parity PROPERTIES
    DEPENDS gpu_backend_parity_reference
    SKIP_REGULAR_EXPRESSION "gpu_backend_parity: skipped")

add_executable(perceptual_metric_tests
    perceptual_metric_tests.cpp)
target_link_libraries(perceptual_metric_tests PRIVATE tessera_golden)
add_test(NAME perceptual_metric COMMAND perceptual_metric_tests)

add_executable(difference_class_tests
    difference_class_tests.cpp)
target_link_libraries(difference_class_tests PRIVATE tessera_golden)
add_test(NAME difference_class COMMAND difference_class_tests)

add_executable(parity_policy_tests
    parity_policy_tests.cpp)
target_link_libraries(parity_policy_tests PRIVATE tessera_gpu_parity)
add_test(NAME parity_policy COMMAND parity_policy_tests)
