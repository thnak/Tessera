# SPDX-License-Identifier: MIT

# Own JSX/TSX front end (ADR-036). Portable C++ only: no OS, thread or filesystem calls, so the
# same library links into the native compiler and the Wasm designer engine.
add_library(tessera_jsx_frontend STATIC
    src/diagnostic.cpp
    src/lexer.cpp
    src/lexer_literals.cpp
    src/jsx_text.cpp
    src/parser.cpp
    src/parser_expr.cpp
    src/parser_jsx.cpp
    src/parser_primary.cpp
    src/parser_literals.cpp
    src/parser_type.cpp
    src/parser_module.cpp
    src/parser_component.cpp
    src/module_dump.cpp
    src/ast_dump.cpp
    src/source_map.cpp
    src/utf8.cpp)

target_include_directories(tessera_jsx_frontend
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(tessera_jsx_frontend PUBLIC cxx_std_23)
