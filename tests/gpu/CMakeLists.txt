# SPDX-License-Identifier: MIT

add_executable(rhi_null_tests
    rhi/rhi_null_tests.cpp)
target_link_libraries(rhi_null_tests PRIVATE tessera_rhi_null)
add_test(NAME rhi_null COMMAND rhi_null_tests)

add_executable(batched2d_tests
    batched2d/batched2d_tests.cpp)
target_link_libraries(batched2d_tests PRIVATE tessera_gpu tessera_rhi_null)
add_test(NAME batched2d COMMAND batched2d_tests)

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
