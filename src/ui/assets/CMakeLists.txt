# SPDX-License-Identifier: MIT

add_library(tessera_assets STATIC
    asset_bundle.cpp
    asset_bundle_compose.cpp)

target_include_directories(tessera_assets PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_assets PUBLIC cxx_std_23)

# The SVG-on-bundle helper lives in the UI assets service because it composes an app-facing asset
# bundle with the rendering path pipeline (UI depends on rendering; rendering never depends on UI).
add_library(tessera_asset_svg STATIC
    asset_svg.cpp)

target_include_directories(tessera_asset_svg PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_asset_svg PUBLIC cxx_std_23)

target_link_libraries(tessera_asset_svg PUBLIC
    tessera_assets
    tessera_path)

# Embedded raster-image loading (PNG/JPEG via stb_image) composed with the bundle, mirroring
# tessera_asset_svg; the decode cache and texture cache live under rendering/image.
add_library(tessera_asset_image STATIC
    asset_image.cpp)

target_include_directories(tessera_asset_image PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_asset_image PUBLIC cxx_std_23)

target_link_libraries(tessera_asset_image PUBLIC
    tessera_assets
    tessera_image)

# Font stacks from a bundle (docs/design/text.md): the app ships its faces and an ordered stack
# manifest as assets, and the host builds the FallbackShaper from them.
add_library(tessera_asset_font STATIC
    asset_font.cpp)

target_include_directories(tessera_asset_font PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_asset_font PUBLIC cxx_std_23)

target_link_libraries(tessera_asset_font PUBLIC
    tessera_assets
    tessera_text)

include(${PROJECT_SOURCE_DIR}/cmake/TesseraAssets.cmake)

# Asset tests are registered from this subdirectory so the lane does not edit the shared
# tests/unit registry while other lanes are working in it.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    set(TESSERA_ASSET_FIXTURE_DIR "${PROJECT_SOURCE_DIR}/tests/fixtures/assets")

    add_executable(asset_image_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/asset_image_tests.cpp)
    target_link_libraries(asset_image_tests PRIVATE tessera_asset_image)
    target_compile_definitions(asset_image_tests PRIVATE
        TESSERA_IMAGE_FIXTURE_DIR="${TESSERA_ASSET_FIXTURE_DIR}")
    add_test(NAME asset_image COMMAND asset_image_tests)

    add_executable(asset_font_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/asset_font_tests.cpp)
    tessera_embed_assets(asset_font_tests
        MANIFEST ${TESSERA_ASSET_FIXTURE_DIR}/fonts.manifest
        NAMESPACE tessera::ui::assets::font_fixtures
        STEM asset_font_fixtures)
    target_link_libraries(asset_font_tests PRIVATE tessera_asset_font)
    add_test(NAME asset_font COMMAND asset_font_tests)

    add_executable(asset_bundle_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/asset_bundle_tests.cpp)
    tessera_embed_assets(asset_bundle_tests
        MANIFEST ${TESSERA_ASSET_FIXTURE_DIR}/icons.manifest
        NAMESPACE tessera::ui::assets::fixtures)
    target_link_libraries(asset_bundle_tests PRIVATE
        tessera_asset_svg
        tessera_path
        tessera_rhi_null)
    target_compile_definitions(asset_bundle_tests PRIVATE
        TESSERA_ASSET_FIXTURE_DIR="${TESSERA_ASSET_FIXTURE_DIR}"
        TESSERA_SVG_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/svg")
    add_test(NAME asset_bundle COMMAND asset_bundle_tests)

    add_executable(asset_zero_bundle_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/asset_zero_bundle_tests.cpp)
    tessera_embed_assets(asset_zero_bundle_tests
        MANIFEST ${TESSERA_ASSET_FIXTURE_DIR}/empty.manifest
        NAMESPACE tessera::ui::assets::empty_fixtures)
    target_link_libraries(asset_zero_bundle_tests PRIVATE tessera_assets)
    add_test(NAME asset_zero_bundle COMMAND asset_zero_bundle_tests)

    # Composition over several bundles: first-bundle-wins precedence, value failures, and the
    # vector asset chain (composed lookup -> SVG decode) an app uses when it embeds both a generated
    # vector bundle and a project manifest bundle.
    add_executable(asset_bundle_compose_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/asset_bundle_compose_tests.cpp)
    target_link_libraries(asset_bundle_compose_tests PRIVATE
        tessera_asset_svg
        tessera_path)
    add_test(NAME asset_bundle_compose COMMAND asset_bundle_compose_tests)

    add_test(
        NAME asset_generator
        COMMAND ${CMAKE_COMMAND}
            -DEMBED_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/embed_assets.cmake
            -DMANIFEST=${TESSERA_ASSET_FIXTURE_DIR}/icons.manifest
            -DDUPLICATE_MANIFEST=${TESSERA_ASSET_FIXTURE_DIR}/duplicate.manifest
            -DINVALID_MANIFEST=${TESSERA_ASSET_FIXTURE_DIR}/invalid_name.manifest
            -DGOLDEN_DIR=${PROJECT_SOURCE_DIR}/tests/golden/assets
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/asset_generator
            -P ${PROJECT_SOURCE_DIR}/tests/unit/asset_gen_test.cmake)
endif()
