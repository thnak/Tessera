# SPDX-License-Identifier: MIT

add_executable(batched2d_poc
    main.cpp)

target_link_libraries(batched2d_poc PRIVATE
    tessera_gpu
    tessera_hal_window
    tessera_runtime
    tessera_frame
    tessera_input
    tessera_rhi_vulkan
    tessera_rhi_null
    tessera_software
    tessera_text_render
    tessera_diagnostics
    tessera_golden
    tessera_stb
    tessera_path
    tessera_image
    tessera_asset_svg
    tessera_asset_image)

target_compile_features(batched2d_poc PRIVATE cxx_std_23)

# The CPU vector golden paints embedded assets; the manifest is generated on the build host by the
# CMake embed script (works in cross builds too).
include(${PROJECT_SOURCE_DIR}/cmake/TesseraAssets.cmake)
tessera_embed_assets(batched2d_poc
    MANIFEST ${PROJECT_SOURCE_DIR}/tests/fixtures/assets/icons.manifest
    NAMESPACE tessera::poc::batched2d::fixtures)
tessera_embed_assets(batched2d_poc
    MANIFEST ${PROJECT_SOURCE_DIR}/tests/fixtures/assets/images.manifest
    NAMESPACE tessera::poc::batched2d::image_fixtures
    STEM batched2d_poc_image)

# `--simulate-loss` needs the Vulkan test hook (same guard the backend/tests use). The include
# directories/definitions mirror the internal-hook test targets; without hooks the flag is skipped.
if (TESSERA_VULKAN_TEST_HOOKS)
    target_include_directories(batched2d_poc PRIVATE
        ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/src
        ${PROJECT_SOURCE_DIR}/third_party/volk
        ${PROJECT_SOURCE_DIR}/third_party/vma)
    target_compile_definitions(batched2d_poc PRIVATE VK_NO_PROTOTYPES)
    if (WIN32)
        target_compile_definitions(batched2d_poc PRIVATE VK_USE_PLATFORM_WIN32_KHR)
    elseif (TESSERA_X11_FOUND)
        target_compile_definitions(batched2d_poc PRIVATE VK_USE_PLATFORM_XLIB_KHR)
        target_link_libraries(batched2d_poc PRIVATE X11::X11)
    endif()
endif()
