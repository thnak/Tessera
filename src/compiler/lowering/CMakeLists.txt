# SPDX-License-Identifier: MIT

# Tier 2 expression lowering (docs/design/jsx-ir-lowering.md section 9, slice 5). It reads the AST
# and builds IR, so it links the front end and the IR, and nothing else from the project.
add_library(tessera_lowering STATIC
    src/call_lowering.cpp
    src/expression_lowering.cpp
    src/literal_lowering.cpp
    src/reference_lowering.cpp
    src/source_span.cpp
    src/template_lowering.cpp)

target_include_directories(tessera_lowering
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(tessera_lowering PUBLIC tessera_jsx_frontend tessera_ir)
target_compile_features(tessera_lowering PUBLIC cxx_std_23)

# Layering rule of docs/design/jsx-ir-lowering.md section 6: lowering is the only layer that sees
# both the AST and the IR. Keep the dependency list closed.
get_target_property(_tessera_lowering_links tessera_lowering LINK_LIBRARIES)
if (NOT _tessera_lowering_links STREQUAL "tessera_jsx_frontend;tessera_ir")
    message(FATAL_ERROR
        "tessera_lowering must link exactly tessera_jsx_frontend and tessera_ir, found: "
        "${_tessera_lowering_links}")
endif ()
