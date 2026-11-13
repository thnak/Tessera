# SPDX-License-Identifier: MIT

add_library(tessera_paint STATIC
    paint_cache.cpp
    paint_pass.cpp
    paint_image.cpp
    paint_properties.cpp
    paint_shadow.cpp
    paint_sinks.cpp
    paint_text.cpp
    paint_text_metrics.cpp
    paint_text_runs.cpp
    scene_style_sync.cpp
    paint_vector.cpp
    text_layout_cache.cpp)

target_include_directories(tessera_paint PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_paint PUBLIC
    tessera_scene
    tessera_style
    tessera_gpu
    tessera_text_render
    tessera_asset_svg
    tessera_asset_image
    tessera_image
    tessera_path)

target_compile_features(tessera_paint PUBLIC cxx_std_23)

# Vector PaintData tests are registered from this subdirectory so the lane does not edit the shared
# tests/unit registry while other lanes are working in it.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    include(${PROJECT_SOURCE_DIR}/cmake/TesseraAssets.cmake)

    add_executable(paint_vector_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/paint_vector_tests.cpp)
    target_link_libraries(paint_vector_tests PRIVATE
        tessera_paint
        tessera_rhi_null)
    add_test(NAME paint_vector COMMAND paint_vector_tests)

    add_executable(paint_vector_sink_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/paint_vector_sink_tests.cpp)
    target_link_libraries(paint_vector_sink_tests PRIVATE
        tessera_paint
        tessera_rhi_null)
    add_test(NAME paint_vector_sink COMMAND paint_vector_sink_tests)

    # Vulkan-free CPU vector golden: embedded SVG assets -> CPU raster -> textured-quad blit,
    # compared against the committed baseline at tolerance 0. Runs on every platform whose build
    # has ThorVG; with ThorVG OFF the scene prints the documented skip line and CTest skips it.
    add_executable(cpu_vector_golden_tests
        ${PROJECT_SOURCE_DIR}/tests/gpu/cpu_vector_golden_tests.cpp)
    target_include_directories(cpu_vector_golden_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
    tessera_embed_assets(cpu_vector_golden_tests
        MANIFEST ${PROJECT_SOURCE_DIR}/tests/fixtures/assets/icons.manifest
        NAMESPACE tessera::poc::batched2d::fixtures)
    target_link_libraries(cpu_vector_golden_tests PRIVATE
        tessera_paint
        tessera_software
        tessera_path
        tessera_asset_svg
        tessera_golden
        tessera_stb)
    add_test(NAME batched2d_cpu_vector_golden
        COMMAND cpu_vector_golden_tests
            ${PROJECT_SOURCE_DIR}/tests/golden/baseline/batched2d/dashboard_vector_cpu.png)
    if (NOT TESSERA_ENABLE_THORVG)
        set_tests_properties(batched2d_cpu_vector_golden PROPERTIES
            SKIP_REGULAR_EXPRESSION "cpu-vector-golden: skipped")
    endif()

    # Vulkan-free pixel check of box-shadow through the CPU sink (blur falloff, sharp offset).
    add_executable(paint_shadow_cpu_tests
        ${PROJECT_SOURCE_DIR}/tests/gpu/paint_shadow_cpu_tests.cpp)
    target_include_directories(paint_shadow_cpu_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
    target_link_libraries(paint_shadow_cpu_tests PRIVATE
        tessera_paint
        tessera_software
        tessera_path
        tessera_asset_svg
        tessera_image
        tessera_asset_image)
    add_test(NAME paint_shadow_cpu COMMAND paint_shadow_cpu_tests)

    # Glass paint pass (issue #244): segmentation at glass nodes, sigma clamp, webkit alias,
    # high-contrast fallback, paint-cache replay and the CPU reference sink's unsupported
    # diagnostic. Vulkan-free, so it runs on every lane.
    add_executable(paint_glass_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/glass_paint_tests.cpp)
    target_include_directories(paint_glass_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
    target_link_libraries(paint_glass_tests PRIVATE
        tessera_paint
        tessera_software
        tessera_path
        tessera_asset_svg
        tessera_image
        tessera_asset_image)
    add_test(NAME paint_glass COMMAND paint_glass_tests)

    # Glass damage, idle and diagnostics (glass-v1 sections 7 and 9): 3 sigma dependency region,
    # static glass screen presenting zero frames, non-glass damage unchanged, glass counters.
    add_executable(glass_damage_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/glass_damage_tests.cpp)
    target_link_libraries(glass_damage_tests PRIVATE
        tessera_frame
        tessera_layout
        tessera_paint
        tessera_runtime)
    add_test(NAME glass_damage COMMAND glass_damage_tests)

    add_executable(paint_image_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/paint_image_tests.cpp)
    target_link_libraries(paint_image_tests PRIVATE
        tessera_paint
        tessera_rhi_null)
    add_test(NAME paint_image COMMAND paint_image_tests)

    # Per-node paint cache: byte-identical replay, clean-subtree zero re-record, invalidation matrix,
    # bounds/LRU/byte budgets and determinism. Registered here, not in the shared tests/unit registry,
    # for the same lane-isolation reason as the other paint tests above.
    add_executable(paint_cache_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/paint_cache_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/paint_cache_bounds_tests.cpp)
    target_link_libraries(paint_cache_tests PRIVATE tessera_paint)
    add_test(NAME paint_cache COMMAND paint_cache_tests)

    # Vulkan-free CPU raster-image golden: embedded PNG -> decode cache -> textured blit, compared
    # against the committed baseline at tolerance 0. JPEG is decode-tested only (cross-compiler IDCT
    # last bits are not a tolerance-0 golden candidate; documented in the image-assets note).
    add_executable(cpu_image_golden_tests
        ${PROJECT_SOURCE_DIR}/tests/gpu/cpu_image_golden_tests.cpp)
    target_include_directories(cpu_image_golden_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/tools/poc/batched2d)
    tessera_embed_assets(cpu_image_golden_tests
        MANIFEST ${PROJECT_SOURCE_DIR}/tests/fixtures/assets/images.manifest
        NAMESPACE tessera::poc::batched2d::image_fixtures)
    target_link_libraries(cpu_image_golden_tests PRIVATE
        tessera_paint
        tessera_software
        tessera_image
        tessera_asset_image
        tessera_golden
        tessera_stb)
    add_test(NAME batched2d_cpu_image_golden
        COMMAND cpu_image_golden_tests
            ${PROJECT_SOURCE_DIR}/tests/golden/baseline/batched2d/dashboard_image_cpu.png)


    # GPU readback test (embedded asset painted on scene nodes). Registered from this subdirectory
    # for the same shared-registry reason; tessera_rhi_vulkan is defined later in the root but CMake
    # resolves target names at generate time.
    if (TESSERA_BUILD_VULKAN)
        # GPU image readback: embedded PNG painted on scene nodes, validation clean, plus measurements.
        add_executable(paint_image_gpu_tests
            ${PROJECT_SOURCE_DIR}/tests/gpu/text/paint_image_tests.cpp)
        tessera_embed_assets(paint_image_gpu_tests
            MANIFEST ${PROJECT_SOURCE_DIR}/tests/fixtures/assets/images.manifest
            NAMESPACE tessera::ui::paint::paint_image_gpu_fixtures)
        target_link_libraries(paint_image_gpu_tests PRIVATE
            tessera_paint
            tessera_image
            tessera_asset_image
            tessera_gpu
            tessera_rhi_vulkan)
        add_test(NAME paint_image_gpu COMMAND paint_image_gpu_tests)
        set_tests_properties(paint_image_gpu PROPERTIES
            FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")

        add_executable(paint_vector_gpu_tests
            ${PROJECT_SOURCE_DIR}/tests/gpu/text/paint_vector_tests.cpp)
        tessera_embed_assets(paint_vector_gpu_tests
            MANIFEST ${PROJECT_SOURCE_DIR}/tests/fixtures/assets/icons.manifest
            NAMESPACE tessera::ui::paint::fixtures)
        target_link_libraries(paint_vector_gpu_tests PRIVATE
            tessera_paint
            tessera_asset_svg
            tessera_gpu
            tessera_rhi_vulkan)
        add_test(NAME paint_vector_gpu COMMAND paint_vector_gpu_tests)
        set_tests_properties(paint_vector_gpu PROPERTIES
            FAIL_REGULAR_EXPRESSION "\\[vulkan\\]")
    endif()
endif()
