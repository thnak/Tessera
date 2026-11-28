# SPDX-License-Identifier: MIT

# Symbol and span index over the front end (issue #570, ADR-060 section 3): declarations,
# references and UTF-16 spans with stable ids, the foundation of definition, references, hover,
# completions and rename. Portable C++ only (no OS, thread or filesystem calls), so the same
# library links into the native hosts and the Wasm analysis worker.
add_library(tessera_analysis STATIC
    src/completion.cpp
    src/css_scan.cpp
    src/diagnostics.cpp
    src/file_indexer.cpp
    src/file_indexer_expr.cpp
    src/language.cpp
    src/project_index.cpp
    src/rename.cpp
    src/symbol_json.cpp
    src/utf16_map.cpp)

target_include_directories(tessera_analysis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(tessera_analysis PUBLIC tessera_jsx_frontend)
target_compile_features(tessera_analysis PUBLIC cxx_std_23)

# The analysis worker's C ABI (src/analysis_entry.h). On a host build it is a static library, so the
# ordinary lanes type-check it and the parity test drives the very functions the page calls; the
# Wasm lane links the module. ES6 modularized with no entry point and no NODERAWFS, like
# tessera_format (tools/fmt/CMakeLists.txt, docs/design/compiler-wasm.md).
if (NOT EMSCRIPTEN)
    add_library(tessera_analysis_entry STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/analysis_entry.cpp)
    target_include_directories(tessera_analysis_entry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(tessera_analysis_entry PUBLIC tessera_analysis)
else ()
    add_executable(tessera_analysis_wasm ${CMAKE_CURRENT_SOURCE_DIR}/src/analysis_entry.cpp)
    target_link_libraries(tessera_analysis_wasm PRIVATE tessera_analysis)
    set_target_properties(tessera_analysis_wasm PROPERTIES OUTPUT_NAME tessera_analysis)
    target_link_options(tessera_analysis_wasm PRIVATE
        --no-entry
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createTesseraAnalysis
        -sENVIRONMENT=web,worker,node
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORTED_FUNCTIONS=["_tessera_analysis_reset","_tessera_analysis_set_file","_tessera_analysis_commit_snapshot","_tessera_analysis_edit","_tessera_analysis_commit_edits","_tessera_analysis_query_json","_tessera_analysis_request_json","_tessera_analysis_rename_json","_tessera_free","_malloc","_free"]
        -sEXPORTED_RUNTIME_METHODS=["UTF8ToString","lengthBytesUTF8","stringToUTF8"])
endif ()
