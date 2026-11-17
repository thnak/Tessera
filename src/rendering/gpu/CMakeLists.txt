# SPDX-License-Identifier: MIT

add_library(tessera_gpu STATIC
    batched2d_renderer.cpp
    # The 4x presentation seam (S8): a sampled-quad pass that presents a multisampled frame through
    # its resolved image. Split out so the renderer core stays inside its size budget.
    batched2d_present.cpp
    batched2d_cull.cpp
    batched2d_seam.cpp
    clip_state_table.cpp
    glass_blur.cpp
    glass_compositor.cpp
    offscreen_target_pool.cpp
    surface_renderer.cpp
    vertex_arena.cpp)

target_include_directories(tessera_gpu PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

# `SurfaceRenderer` composites into a `hal::NativeSurface` (ADR-050 section 7, AF6), so this library
# links the HAL's surface seam. That is an upward edge from the rendering layer to the HAL, which
# already exists (`src/app` links both) and is the intended one: the HAL boundary is *below*
# rendering, and the surface type is the only thing rendering borrows from it.
target_link_libraries(tessera_gpu PUBLIC tessera_rhi tessera_damage tessera_hal_surface)

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

    # The three state clusters extracted from Batched2DRenderer (god-type audit slice 8):
    # ClipStateTable, VertexArena and GlassCompositor, tested directly rather than through a command
    # stream. Registered here for the same reason as glass_blur above.
    add_executable(gpu_collaborators_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/gpu_collaborators_tests.cpp)
    target_link_libraries(gpu_collaborators_tests PRIVATE tessera_gpu tessera_rhi_null)
    add_test(NAME gpu_collaborators COMMAND gpu_collaborators_tests)
endif()
