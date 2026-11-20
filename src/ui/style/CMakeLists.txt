# SPDX-License-Identifier: MIT

# The CSS tokenizer is scene-free (it includes only its own header, `<cstdlib>` and `<string>`), so
# it is its own target: the compiler's CSS-module scan tokenizes a module file without linking the
# scene graph, the cascade or the rest of the style engine. The headers stay where they are
# (`include/tessera/ui/style/{token,tokenizer}.hpp`), so the public API baseline does not move.
add_library(tessera_css_tokenizer STATIC tokenizer.cpp)
target_include_directories(tessera_css_tokenizer PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_compile_features(tessera_css_tokenizer PUBLIC cxx_std_23)

add_library(tessera_style STATIC
    backdrop_filter.cpp
    cascade.cpp
    font_size.cpp
    color.cpp
    color_functions.cpp
    gradient.cpp
    background_layers.cpp
    property_background.cpp
    property_text_decoration.cpp
    text_decoration.cpp
    color_names.cpp
    color_oklch.cpp
    component_parts.cpp
    compiled_stylesheet.cpp
    container_query.cpp
    contrast.cpp
    cursor_value.cpp
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
    # The node half of SafeArea (safe-area-v1.md sections 3 and 11, issue #408): the `tsafe` token
    # parsing and the one padding/margin writer the app host AND the design-time preview both call.
    # It lives here so the two layout consumers share one implementation instead of a preview-only
    # copy, which is the "no parallel layout path" rule in issue #408.
    safe_area_style.cpp
    scene_style.cpp
    scroll_style_sync.cpp
    supports.cpp
    selector.cpp
    selector_specificity.cpp
    selector_index.cpp
    selector_match.cpp
    selector_has.cpp
    selector_parts.cpp
    # The selector list printed back from its parsed form: the fallback for a sheet that arrived as
    # a blob, whose authored text is not carried (docs/specs/compiled-stylesheet-v1.md section 5).
    selector_text.cpp
    selector_support.cpp
    theme_asset.cpp
    theme_asset_blob.cpp
    theme_generate.cpp
    theme_generate_json.cpp
    theme_generate_tokens.cpp
    theme_solve.cpp
    theme_stylesheet.cpp
    transform.cpp
    ua_stylesheet.cpp
    var_resolve.cpp)

target_include_directories(tessera_style PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_style PUBLIC tessera_scene)
# The tokenizer split above: `tessera_style` keeps its own tokenizer, and the compiler's CSS-module
# scan links the same one instead of a second copy.
target_link_libraries(tessera_style PUBLIC tessera_css_tokenizer)
# The SafeArea inset arithmetic and the fold-posture mapping are the platform-NEUTRAL half of the
# HAL (`tessera_hal_metrics`), deliberately not `tessera_hal_window`: this library builds for Wasm,
# where no OS window backend exists, and ADR-020's rule is that UI code sees normalized types only.
target_link_libraries(tessera_style PUBLIC tessera_hal_metrics)
# The theme asset is parsed at build time with the harness JSON decoder
# (docs/specs/stylesheet-v1.md section 9.1); the engine never parses it at run time.
target_link_libraries(tessera_style PRIVATE tessera_harness)

target_compile_features(tessera_style PUBLIC cxx_std_23)

# `tessera theme --seed <color>` (issue #79, ADR-047 item 5): the CLI for the token-set generator.
# It is registered from this module, not from the root CMakeLists.txt, on the same precedent as
# `tessera_codegen_cli` in src/compiler/codegen -- the module that owns the library registers the
# tool that drives it. The binary is `tessera_theme` so the future `tessera` driver can forward its
# arguments, exactly as `tessera icons add` does.
add_executable(tessera_theme
    ${PROJECT_SOURCE_DIR}/tools/theme/src/main.cpp)

target_link_libraries(tessera_theme PRIVATE tessera_style)

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

    # The schema inline-style table and the registry must agree (issue #272). This is the only place
    # both are visible: `tessera_schema` may not link the UI module, so the table is hand-maintained
    # and this test is the seam that keeps the two from drifting again. Registered here for the same
    # reason as the lanes above; the link to `tessera_schema` is a test-only edge, not a library one.
    add_executable(schema_style_registry_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/schema_style_registry_tests.cpp)
    target_link_libraries(schema_style_registry_tests PRIVATE tessera_style tessera_schema)
    add_test(NAME schema_style_registry COMMAND schema_style_registry_tests)

    # Theme generation from one seed color (issue #79, ADR-047 item 5). Four lanes, registered
    # here for the same reason as the lanes above:
    #   color_oklch     the OKLCH/sRGB math with published reference vectors
    #   theme_contrast  the WCAG 2.2 checker, its role table and the floors per role
    #   theme_generate  the generator across the hue wheel, determinism and identical token sets
    #   theme_golden    byte goldens for the hard seeds, in tests/golden/theme
    add_executable(color_oklch_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/color_oklch_tests.cpp)
    target_link_libraries(color_oklch_tests PRIVATE tessera_style)
    add_test(NAME color_oklch COMMAND color_oklch_tests)

    add_executable(theme_contrast_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/theme_contrast_tests.cpp)
    target_link_libraries(theme_contrast_tests PRIVATE tessera_style)
    add_test(NAME theme_contrast COMMAND theme_contrast_tests)

    add_executable(theme_generate_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/theme_generate_tests.cpp)
    target_link_libraries(theme_generate_tests PRIVATE tessera_style)
    add_test(NAME theme_generate COMMAND theme_generate_tests)

    add_executable(theme_golden_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/theme_golden_tests.cpp)
    target_link_libraries(theme_golden_tests PRIVATE tessera_style)
    target_compile_definitions(theme_golden_tests PRIVATE
        TESSERA_THEME_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/theme")
    add_test(NAME theme_golden COMMAND theme_golden_tests)

    # CLI contract: --seed writes the asset and is byte-deterministic, --check accepts it and
    # rejects a doctored copy, an invalid seed and an invalid id prefix exit non-zero, and the
    # shipped Instrument asset passes the same lint.
    add_test(NAME theme_cli
        COMMAND ${CMAKE_COMMAND}
            -DTOOL=$<TARGET_FILE:tessera_theme>
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/theme_cli_out
            -DGOLDEN=${PROJECT_SOURCE_DIR}/tests/golden/theme
            -DINSTRUMENT=${PROJECT_SOURCE_DIR}/packages/tessera-ui/themes/instrument.theme.json
            -P ${PROJECT_SOURCE_DIR}/tools/theme/run_cli_tests.cmake)
endif()
