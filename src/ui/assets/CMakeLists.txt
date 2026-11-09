# SPDX-License-Identifier: MIT

add_library(tessera_assets STATIC
    asset_bundle.cpp)

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

include(${PROJECT_SOURCE_DIR}/cmake/TesseraAssets.cmake)

# Asset tests are registered from this subdirectory so the lane does not edit the shared
# tests/unit registry while other lanes are working in it.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    set(TESSERA_ASSET_FIXTURE_DIR "${PROJECT_SOURCE_DIR}/tests/fixtures/assets")

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
