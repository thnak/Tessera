# SPDX-License-Identifier: MIT

# Tier 2 lowering (docs/design/jsx-ir-lowering.md section 9). It reads the AST and the analysis
# result and builds the IR, so it links the front end, the analysis pass and the IR, and nothing
# else from the project.
add_library(tessera_lowering STATIC
    src/binding_lowering.cpp
    src/call_lowering.cpp
    src/component_names.cpp
    src/expression_lowering.cpp
    src/handler_actions.cpp
    src/hook_lowering.cpp
    src/hook_lowering_support.cpp
    src/hook_lookups.cpp
    src/hook_type.cpp
    src/literal_lowering.cpp
    src/reference_lowering.cpp
    src/source_span.cpp
    src/template_lowering.cpp)

target_include_directories(tessera_lowering
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(tessera_lowering PUBLIC tessera_jsx_frontend tessera_ir tessera_react_rules)
target_compile_features(tessera_lowering PUBLIC cxx_std_23)

# Layering rule of docs/design/jsx-ir-lowering.md section 6: lowering is the only layer that sees
# the AST, the analysis and the IR. Keep the dependency list closed.
get_target_property(_tessera_lowering_links tessera_lowering LINK_LIBRARIES)
if (NOT _tessera_lowering_links STREQUAL "tessera_jsx_frontend;tessera_ir;tessera_react_rules")
    message(FATAL_ERROR
        "tessera_lowering must link exactly tessera_jsx_frontend, tessera_ir and "
        "tessera_react_rules, found: ${_tessera_lowering_links}")
endif ()
