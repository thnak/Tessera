# SPDX-License-Identifier: MIT
#
# Backend-selectable GPU conformance suite (issue #161). This directory owns its targets and ctest
# entries so the shared tests/gpu/CMakeLists.txt only needs one add_subdirectory(conformance)
# line. Goldens still come from lavapipe/CPU only; this suite never asserts on pixels, it checks
# the RHI contract and reports pass/skip per backend.

add_library(tessera_gpu_conformance STATIC
    conformance_backend.cpp
    conformance_check.cpp
    conformance_checks.cpp
    conformance_suite.cpp
    conformance_summary.cpp)
target_include_directories(tessera_gpu_conformance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tessera_gpu_conformance PUBLIC
    tessera_rhi
    tessera_rhi_null)
target_compile_features(tessera_gpu_conformance PUBLIC cxx_std_23)

if (TARGET tessera_rhi_vulkan)
    target_link_libraries(tessera_gpu_conformance PUBLIC tessera_rhi_vulkan)
    target_compile_definitions(tessera_gpu_conformance PUBLIC TESSERA_TEST_HAS_VULKAN)
endif()

# Metal (ADR-053). The device half only exists on an Apple build with TESSERA_BUILD_METAL=ON; the
# entry below is registered regardless, so a lane without it reports a named skip rather than
# silently passing.
if (TARGET tessera_rhi_metal)
    target_link_libraries(tessera_gpu_conformance PUBLIC tessera_rhi_metal)
    target_compile_definitions(tessera_gpu_conformance PUBLIC TESSERA_TEST_HAS_METAL)
endif()

add_executable(gpu_conformance
    conformance_runner.cpp)
target_link_libraries(gpu_conformance PRIVATE tessera_gpu_conformance)

add_test(NAME gpu_conformance_null
    COMMAND gpu_conformance --backend null
        --summary "${CMAKE_CURRENT_BINARY_DIR}/conformance_null.json")

# The Vulkan entry is always registered: builds without the Vulkan backend and hosts without a
# device print a "gpu_conformance: skipped" line and exit 0, so CI without a GPU stays green while
# the run is reported as skipped instead of silently passing.
add_test(NAME gpu_conformance_vulkan
    COMMAND gpu_conformance --backend vulkan
        --summary "${CMAKE_CURRENT_BINARY_DIR}/conformance_vulkan.json")
set_tests_properties(gpu_conformance_vulkan PROPERTIES
    SKIP_REGULAR_EXPRESSION "gpu_conformance: skipped")

# WebGPU is selectable (issue #245) but this headless runner has no host-provided WGPUDevice, so
# the entry skips with a named reason on every current lane; webgpu.yml is its build check.
add_test(NAME gpu_conformance_webgpu
    COMMAND gpu_conformance --backend webgpu
        --summary "${CMAKE_CURRENT_BINARY_DIR}/conformance_webgpu.json")
set_tests_properties(gpu_conformance_webgpu PROPERTIES
    SKIP_REGULAR_EXPRESSION "gpu_conformance: skipped")

# Metal (ADR-053) is selectable on every lane; a build without the backend, or a host without a
# Metal device, prints the skip line and exits 0. Tier 2 promotion needs this to pass on a macOS
# lane, which is the evidence the tier table asks for.
add_test(NAME gpu_conformance_metal
    COMMAND gpu_conformance --backend metal
        --summary "${CMAKE_CURRENT_BINARY_DIR}/conformance_metal.json")
set_tests_properties(gpu_conformance_metal PROPERTIES
    SKIP_REGULAR_EXPRESSION "gpu_conformance: skipped")

# Summary determinism and shape: two null runs must serialize byte-identically.
add_executable(conformance_summary_tests
    conformance_summary_tests.cpp)
target_link_libraries(conformance_summary_tests PRIVATE tessera_gpu_conformance)
add_test(NAME gpu_conformance_summary COMMAND conformance_summary_tests)
