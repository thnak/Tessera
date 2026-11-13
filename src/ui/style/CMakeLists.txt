# SPDX-License-Identifier: MIT

add_library(tessera_style STATIC
    backdrop_filter.cpp
    cascade.cpp
    color.cpp
    color_names.cpp
    component_parts.cpp
    compiled_stylesheet.cpp
    container_query.cpp
    grid_values.cpp
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
    property_registry_extra.cpp
    property_value_validation.cpp
    scene_style.cpp
    scroll_style_sync.cpp
    supports.cpp
    selector.cpp
    selector_index.cpp
    selector_match.cpp
    selector_parts.cpp
    selector_support.cpp
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

    # CSS Grid subset (issue #249): the registry entries, the `gap`/`place-self` shorthands, the
    # shared value grammar and the `@supports` answers.
    add_executable(grid_style_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/grid_style_tests.cpp)
    target_link_libraries(grid_style_tests PRIVATE tessera_style)
    add_test(NAME grid_style COMMAND grid_style_tests)

    # `::part()` (issue #235): selector parse, specificity, matching across the component boundary,
    # the undeclared-part diagnostic and the compiled-stylesheet round trip. Registered here for the
    # same reason as the glass lane above.
    add_executable(part_pseudo_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/part_pseudo_tests.cpp)
    target_link_libraries(part_pseudo_tests PRIVATE tessera_style)
    add_test(NAME part_pseudo COMMAND part_pseudo_tests)

    # The `part` attribute path (issue #235): JSX -> IR prop -> generated `set_part`, and the
    # component name a host node carries. Registered here for the same reason.
    add_executable(part_attribute_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/part_attribute_tests.cpp)
    target_link_libraries(part_attribute_tests PRIVATE
        tessera_codegen
        tessera_lowering
        tessera_ir)
    add_test(NAME part_attribute COMMAND part_attribute_tests)

    # The V1 compatibility matrix and the registry must agree (issue #250). The matrix's property
    # list is a machine-readable file next to the spec, so the test reads it instead of parsing the
    # markdown table. Registered here for the same reason as the lanes above.
    add_executable(property_registry_matrix_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/property_registry_matrix_tests.cpp)
    target_link_libraries(property_registry_matrix_tests PRIVATE tessera_style)
    target_compile_definitions(property_registry_matrix_tests PRIVATE
        TESSERA_STYLESHEET_SPEC_DIR="${PROJECT_SOURCE_DIR}/docs/specs")
    add_test(NAME property_registry_matrix COMMAND property_registry_matrix_tests)
endif()
