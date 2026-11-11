# SPDX-License-Identifier: MIT

# Evaluator for IR expressions and action lists (docs/specs/ir-v1.md sections 4.1 and 4.2). It is
# the reference for what generated C++ must compute, so it links only the IR and nothing else from
# the project: no parser, no UI, no renderer.
add_library(tessera_ir_eval STATIC
    src/builtins.cpp
    src/evaluator.cpp
    src/number_text.cpp
    src/operators.cpp
    src/value.cpp)

target_include_directories(tessera_ir_eval
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(tessera_ir_eval PUBLIC cxx_std_23)

target_link_libraries(tessera_ir_eval PUBLIC tessera_ir)
