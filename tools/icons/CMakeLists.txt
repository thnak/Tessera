# SPDX-License-Identifier: MIT

# `tessera icons add` (ADR-047 item 8, docs/design/component-library.md section 3.1): imports icons
# from pinned Iconify sets on disk, gates the set license, converts static icons into the engine's
# vector asset form and tree-shakes the unreferenced ones. The core is a library so the CLI, the
# tests and the golden comparison all exercise the same bytes.
add_library(tessera_icons_core STATIC
    src/icon_emit.cpp
    src/icon_import.cpp
    src/icon_license.cpp
    src/icon_set.cpp
    src/icon_svg.cpp)

target_include_directories(tessera_icons_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(tessera_icons_core PUBLIC cxx_std_23)

# The harness JSON codec is the repository's only JSON dependency (no third-party parser).
target_link_libraries(tessera_icons_core PUBLIC tessera_harness)

add_executable(tessera_icons
    src/main.cpp)

target_link_libraries(tessera_icons PRIVATE tessera_icons_core)

# Tests are registered from this directory (their sources still live under tests/unit) so a lane
# working on the tool never edits the shared tests/unit registry -- the same pattern as
# src/rendering/path. This subdirectory is added before the root enable_testing(), so enable it here.
set(TESSERA_ICONS_FIXTURE_DIR "${PROJECT_SOURCE_DIR}/tests/fixtures/icons")
set(TESSERA_ICONS_GOLDEN_DIR "${PROJECT_SOURCE_DIR}/tests/golden/icons")

if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(icons_license_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/icons_license_tests.cpp)
    target_link_libraries(icons_license_tests PRIVATE tessera_icons_core)
    add_test(NAME icons_license COMMAND icons_license_tests)

    add_executable(icons_import_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/icons_import_tests.cpp)
    target_link_libraries(icons_import_tests PRIVATE tessera_icons_core)
    target_compile_definitions(icons_import_tests PRIVATE
        TESSERA_ICONS_FIXTURE_DIR="${TESSERA_ICONS_FIXTURE_DIR}"
        TESSERA_ICONS_GOLDEN_DIR="${TESSERA_ICONS_GOLDEN_DIR}")
    add_test(NAME icons_import COMMAND icons_import_tests)

    add_executable(icons_gate_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/icons_gate_tests.cpp)
    target_link_libraries(icons_gate_tests PRIVATE tessera_icons_core)
    target_compile_definitions(icons_gate_tests PRIVATE
        TESSERA_ICONS_FIXTURE_DIR="${TESSERA_ICONS_FIXTURE_DIR}")
    add_test(NAME icons_gate COMMAND icons_gate_tests)

    # CLI contract: writes, verifies (--check), refuses a stale directory and refuses a rejected
    # license, all against the pinned fixture sets.
    add_test(NAME icons_cli
        COMMAND ${CMAKE_COMMAND}
            -DTOOL=$<TARGET_FILE:tessera_icons>
            -DFIXTURES=${TESSERA_ICONS_FIXTURE_DIR}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/icons_cli_out
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_cli_tests.cmake)
endif()
