# SPDX-License-Identifier: MIT

# Symbol and span index over the front end (issue #570, ADR-060 section 3): declarations,
# references and UTF-16 spans with stable ids, the foundation of definition, references, hover,
# completions and rename. Portable C++ only (no OS, thread or filesystem calls), so the same
# library links into the native hosts and the Wasm analysis worker.
add_library(tessera_analysis STATIC
    src/file_indexer.cpp
    src/file_indexer_expr.cpp
    src/project_index.cpp
    src/symbol_json.cpp
    src/utf16_map.cpp)

target_include_directories(tessera_analysis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(tessera_analysis PUBLIC tessera_jsx_frontend)
target_compile_features(tessera_analysis PUBLIC cxx_std_23)
