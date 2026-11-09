# SPDX-License-Identifier: MIT

# Compiler IR (docs/specs/ir-v1.md). Plain data plus a JSON writer. It links nothing from the
# project: codegen and tools consume the IR without seeing the parser.
add_library(tessera_ir STATIC
    src/ir.cpp
    src/ir_json.cpp
    src/ir_json_node.cpp
    src/json_value.cpp
    src/validate.cpp
    src/validate_component.cpp
    src/validate_expr.cpp
    src/validate_node.cpp)

target_include_directories(tessera_ir
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(tessera_ir PUBLIC cxx_std_23)

# Layering rule of docs/design/jsx-ir-lowering.md section 6: no project library below the IR.
get_target_property(_tessera_ir_links tessera_ir LINK_LIBRARIES)
if (_tessera_ir_links)
    message(FATAL_ERROR "tessera_ir must not link any library, found: ${_tessera_ir_links}")
endif ()
