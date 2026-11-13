# SPDX-License-Identifier: MIT

add_library(tessera_style STATIC
    backdrop_filter.cpp
    cascade.cpp
    color.cpp
    color_names.cpp
    compiled_stylesheet.cpp
    container_query.cpp
    interaction_style.cpp
    interaction_values.cpp
    keyframes.cpp
    logical_properties.cpp
    media_query.cpp
    parser.cpp
    prepared_stylesheet.cpp
    stylesheet_blob_read.cpp
    stylesheet_blob_write.cpp
    property_registry.cpp
    property_value_validation.cpp
    scene_style.cpp
    scroll_style_sync.cpp
    supports.cpp
    selector.cpp
    selector_match.cpp
    theme_asset.cpp
    theme_asset_blob.cpp
    theme_stylesheet.cpp
    tokenizer.cpp
    transform.cpp
    var_resolve.cpp)

target_include_directories(tessera_style PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_style PUBLIC tessera_scene)
# The theme asset is parsed at build time with the harness JSON decoder
# (docs/specs/stylesheet-v1.md section 9.1); the engine never parses it at run time.
target_link_libraries(tessera_style PRIVATE tessera_harness)

target_compile_features(tessera_style PUBLIC cxx_std_23)

# Glass style surface (issue #244): grammar, UnsupportedValue, initial value, alias and @supports.
# Registered here, not in the shared tests/unit registry, so a lane adding tests never edits a file
# another lane is working in.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(glass_style_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/glass_style_tests.cpp)
    target_link_libraries(glass_style_tests PRIVATE tessera_style)
    add_test(NAME glass_style COMMAND glass_style_tests)

    # Interaction properties (issue #247): pointer-events in hit testing, user-select as the
    # selection gate, @supports answers and the unsupported-value diagnostics.
    add_executable(interaction_style_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/interaction_style_tests.cpp)
    target_link_libraries(interaction_style_tests PRIVATE tessera_style)
    add_test(NAME interaction_style COMMAND interaction_style_tests)
endif()
