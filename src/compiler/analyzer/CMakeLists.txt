# SPDX-License-Identifier: MIT

# Own JSX/TSX front end (ADR-036). Portable C++ only: no OS, thread or filesystem calls, so the
# same library links into the native compiler and the Wasm designer engine.
add_library(tessera_jsx_frontend STATIC
    src/diagnostic.cpp
    src/lexer.cpp
    src/lexer_literals.cpp
    src/jsx_text.cpp
    src/json.cpp
    src/hook_manifest.cpp
    src/parser.cpp
    src/parser_expr.cpp
    src/parser_jsx.cpp
    src/parser_jsx_attributes.cpp
    src/parser_primary.cpp
    src/parser_literals.cpp
    src/parser_type.cpp
    src/parser_type_expand.cpp
    src/parser_type_decls.cpp
    src/parser_module.cpp
    src/parser_component.cpp
    src/parser_reducer.cpp
    src/parser_hook.cpp
    src/hook_expansion.cpp
    src/hook_expansion_call.cpp
    src/hook_expansion_bind.cpp
    src/hook_expansion_clone.cpp
    src/hook_expansion_substitute.cpp
    src/module_dump.cpp
    src/parser_rules.cpp
    src/bind_pairing.cpp
    src/ast_dump.cpp
    src/source_map.cpp
    src/utf8.cpp)

target_include_directories(tessera_jsx_frontend
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(tessera_jsx_frontend PUBLIC cxx_std_23)

# The deterministic limits/stress corpus (docs/design/security.md section 3). It is registered
# here, next to the limits it exercises, so this lane does not depend on tests/unit/CMakeLists.txt;
# the source still lives with the other front-end tests. enable_testing() is idempotent and makes
# CTest pick this subdirectory's test list up even though the root enables testing later.
if (TESSERA_BUILD_TESTS)
    enable_testing()
    add_executable(compiler_limits_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/compiler_limits_tests.cpp)
    target_link_libraries(compiler_limits_tests PRIVATE tessera_jsx_frontend)
    add_test(NAME compiler_limits COMMAND compiler_limits_tests)

    # The diagnostic-code registry golden: one fixture per stable code, the committed golden, and
    # a source scan for codes that have no fixture or skip entry. Registered here for the same
    # reason as the limits corpus: the lane owns its test list, the source lives with the other
    # front-end tests.
    #
    # The registry spans the front end and the Tier-1 rules (the `TESSERA_REACT_*` checks that are
    # not parser-level), so this test links both stages. The analyzer library itself still links
    # only the front end: the rules edge is test-only.
    add_executable(jsx_diagnostics_registry_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/jsx_diagnostics_registry_tests.cpp)
    target_link_libraries(jsx_diagnostics_registry_tests PRIVATE
        tessera_jsx_frontend
        tessera_react_rules)
    target_compile_definitions(jsx_diagnostics_registry_tests PRIVATE
        TESSERA_COMPILER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src/compiler"
        TESSERA_JSX_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/jsx")
    add_test(NAME jsx_diagnostics_registry COMMAND jsx_diagnostics_registry_tests)

    add_executable(hook_manifest_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/hook_manifest_tests.cpp)
    target_link_libraries(hook_manifest_tests PRIVATE tessera_jsx_frontend)
    target_compile_definitions(hook_manifest_tests PRIVATE
        TESSERA_HOOKS_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/hooks-manifest.json")
    add_test(NAME hook_manifest COMMAND hook_manifest_tests)
endif()
