# SPDX-License-Identifier: MIT

add_library(tessera_paint STATIC
    paint_pass.cpp
    paint_properties.cpp
    paint_sinks.cpp
    paint_text_metrics.cpp
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
    tessera_path)

target_compile_features(tessera_paint PUBLIC cxx_std_23)

# Vector PaintData tests are registered from this subdirectory so the lane does not edit the shared
# tests/unit registry while other lanes are working in it.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(paint_vector_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/paint_vector_tests.cpp)
    target_link_libraries(paint_vector_tests PRIVATE
        tessera_paint
        tessera_rhi_null)
    add_test(NAME paint_vector COMMAND paint_vector_tests)

    # GPU readback test (embedded asset painted on scene nodes). Registered from this subdirectory
    # for the same shared-registry reason; tessera_rhi_vulkan is defined later in the root but CMake
    # resolves target names at generate time.
    if (TESSERA_BUILD_VULKAN)
        include(${PROJECT_SOURCE_DIR}/cmake/TesseraAssets.cmake)
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
