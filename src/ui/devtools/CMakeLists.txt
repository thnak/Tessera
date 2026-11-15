# SPDX-License-Identifier: MIT

# The dev-mode compiler-error overlay (issue #78 item 4): a consumer of the
# `compiler-diagnostics-v1` document that renders into the app's existing OverlayHost.
#
# It owns its targets here so parallel slices never edit a root subdirectory list at the same time.
# Layering: this is UI-side presentation, so it links the scene and the harness JSON decoder (the
# same decoder the stylesheet theme asset uses, not a new dependency) and never the codegen CLI: the
# document arrives as text, which is what keeps native and browser diagnostics one format.

add_library(tessera_devtools STATIC
    compiler_error_overlay.cpp)

target_include_directories(tessera_devtools PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_devtools PUBLIC
    tessera_boundary
    tessera_harness
    tessera_scene)

target_compile_features(tessera_devtools PUBLIC cxx_std_23)

if (TESSERA_BUILD_TESTS)
    enable_testing()

    # The document parse, the rendered lines and the overlay entry over a real OverlayHost (so the
    # layer, dismissal and "one panel, not a pile" rules are the shipped ones).
    add_executable(compiler_error_overlay_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/compiler_error_overlay_tests.cpp)
    target_link_libraries(compiler_error_overlay_tests PRIVATE
        tessera_app
        tessera_devtools
        tessera_runtime)
    add_test(NAME compiler_error_overlay COMMAND compiler_error_overlay_tests)

    # The document half, kept separate so neither test file owns a second copy of the spec's example
    # documents and neither outgrows the size limit.
    add_executable(compiler_error_overlay_document_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/compiler_error_overlay_document_tests.cpp)
    target_link_libraries(compiler_error_overlay_document_tests PRIVATE tessera_devtools)
    add_test(NAME compiler_error_overlay_document COMMAND compiler_error_overlay_document_tests)
endif ()
