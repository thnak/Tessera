# SPDX-License-Identifier: MIT

# The compiler-owned source formatter (ADR-061). Portable C++ like the front end it sits on: no OS
# or filesystem calls, so the same code can serve the CLI, the studio worker and the MCP server.
add_library(tessera_source_format STATIC
    build_code.cpp
    build_groups.cpp
    build_statements.cpp
    build_comments.cpp
    build_jsx.cpp
    css_format.cpp
    css_verify.cpp
    json_format.cpp
    json_parse.cpp
    doc.cpp
    edits.cpp
    format.cpp
    recover.cpp
    scan.cpp)

target_include_directories(tessera_source_format
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(tessera_source_format PUBLIC tessera_jsx_frontend tessera_css_tokenizer)
target_compile_features(tessera_source_format PUBLIC cxx_std_23)
