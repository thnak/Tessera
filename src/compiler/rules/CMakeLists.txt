# SPDX-License-Identifier: MIT

# Tier 1 analysis of the JSX AST (docs/design/jsx-ir-lowering.md section 3). It links the front end
# only: no IR, so the checks stay independent of lowering. Portable C++, no OS or thread calls.
add_library(tessera_react_rules STATIC
    src/analyze.cpp
    src/arrows.cpp
    src/context.cpp
    src/dependency_names.cpp
    src/effect_deps.cpp
    src/expr_walk.cpp
    src/hook_placement.cpp
    src/hooks.cpp
    src/memo_deps.cpp
    src/reducer.cpp
    src/resource.cpp
    src/service.cpp)

target_include_directories(tessera_react_rules
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(tessera_react_rules PUBLIC tessera_jsx_frontend tessera_services_types)

# The built-in hook manifest is embedded in the front end (jsx-subset-v1.md section 7.1): the native
# compiler and the Wasm designer both contain it, so neither reads it from disk at run time. The
# generated header is a raw string literal, so the manifest's own quotes need no escaping.
set(TESSERA_HOOKS_MANIFEST_JSON "${PROJECT_SOURCE_DIR}/src/compiler/analyzer/hooks-manifest.json")
file(READ "${TESSERA_HOOKS_MANIFEST_JSON}" TESSERA_HOOKS_MANIFEST_TEXT)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/hooks_manifest_text.inc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/hooks_manifest_text.inc"
    @ONLY)
target_include_directories(tessera_react_rules PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_features(tessera_react_rules PUBLIC cxx_std_23)

# Layering rule of docs/design/jsx-ir-lowering.md section 6: analysis never links the IR.
get_target_property(_tessera_rules_links tessera_react_rules LINK_LIBRARIES)
if (_tessera_rules_links MATCHES "tessera_ir")
    message(FATAL_ERROR "tessera_react_rules must not link tessera_ir, found: ${_tessera_rules_links}")
endif ()
