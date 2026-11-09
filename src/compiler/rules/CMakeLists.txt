# SPDX-License-Identifier: MIT

# Tier 1 analysis of the JSX AST (docs/design/jsx-ir-lowering.md section 3). It links the front end
# only: no IR, so the checks stay independent of lowering. Portable C++, no OS or thread calls.
add_library(tessera_react_rules STATIC
    src/analyze.cpp
    src/arrows.cpp
    src/effect_deps.cpp
    src/expr_walk.cpp
    src/hook_placement.cpp
    src/hooks.cpp)

target_include_directories(tessera_react_rules
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(tessera_react_rules PUBLIC tessera_jsx_frontend)
target_compile_features(tessera_react_rules PUBLIC cxx_std_23)

# Layering rule of docs/design/jsx-ir-lowering.md section 6: analysis never links the IR.
get_target_property(_tessera_rules_links tessera_react_rules LINK_LIBRARIES)
if (_tessera_rules_links MATCHES "tessera_ir")
    message(FATAL_ERROR "tessera_react_rules must not link tessera_ir, found: ${_tessera_rules_links}")
endif ()
