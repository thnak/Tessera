# SPDX-License-Identifier: MIT

# Evaluator for IR expressions and action lists (docs/specs/ir-v1.md sections 4.1 and 4.2). It is
# the reference for what generated C++ must compute, so it links only the IR and nothing else from
# the project: no parser, no UI, no renderer.
add_library(tessera_ir_eval STATIC
    src/builtins.cpp
    src/evaluator.cpp
    src/number_text.cpp
    src/operators.cpp
    src/pending.cpp
    src/value.cpp)

target_include_directories(tessera_ir_eval
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        # ADR-067 spike #661: the pending-UI model (`tessera/ui/pending/`) is header-only and
        # shared with generated C++, so the evaluator includes it from the project include root
        # rather than growing a link edge to the scene.
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(tessera_ir_eval PUBLIC cxx_std_23)

target_link_libraries(tessera_ir_eval PUBLIC tessera_ir)
