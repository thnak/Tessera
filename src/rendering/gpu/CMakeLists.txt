# SPDX-License-Identifier: MIT

add_library(tessera_gpu STATIC
    batched2d_renderer.cpp
    batched2d_clip_state.cpp
    batched2d_glass.cpp
    batched2d_seam.cpp
    glass_blur.cpp
    offscreen_target_pool.cpp)

target_include_directories(tessera_gpu PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_gpu PUBLIC tessera_rhi tessera_damage)

target_compile_features(tessera_gpu PUBLIC cxx_std_23)

# Glass blur kernel/limits (issue #244): normalization, linear-sampling tap count, radius clamp,
# half-resolution decision and the 3 sigma dependency region. Registered here, not in the shared
# tests/unit registry, so this lane never edits a file another lane is working in.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(glass_blur_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/glass_blur_tests.cpp)
    target_link_libraries(glass_blur_tests PRIVATE tessera_gpu)
    add_test(NAME glass_blur COMMAND glass_blur_tests)
endif()
