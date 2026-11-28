# SPDX-License-Identifier: MIT

# `tessera fmt [--check] [paths]` (ADR-061 section 5). Until the unified `tessera` command of
# ADR-077 exists, this is its own binary and accepts `fmt` as an optional first argument, like
# `tessera port-check` does, so the spelling in the docs works on either.
add_library(tessera_fmt_core STATIC ${CMAKE_CURRENT_SOURCE_DIR}/fmt_run.cpp)
target_include_directories(tessera_fmt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tessera_fmt_core PUBLIC tessera_source_format)
target_compile_features(tessera_fmt_core PUBLIC cxx_std_23)

add_executable(tessera_fmt ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
target_link_libraries(tessera_fmt PRIVATE tessera_fmt_core)
set_target_properties(tessera_fmt PROPERTIES OUTPUT_NAME tessera_fmt)

# The browser format module (the studio's format worker, ADR-060). On a host build the entry is an
# OBJECT library so the ordinary lanes type-check it; the Wasm lane links the module. ES6 modularized
# with no entry point and no NODERAWFS, like the other preview modules (docs/design/compiler-wasm.md).
if (NOT EMSCRIPTEN)
    add_library(tessera_format_entry OBJECT ${CMAKE_CURRENT_SOURCE_DIR}/wasm_format_entry.cpp)
    target_link_libraries(tessera_format_entry PRIVATE tessera_source_format)
else ()
    add_executable(tessera_format_wasm ${CMAKE_CURRENT_SOURCE_DIR}/wasm_format_entry.cpp)
    target_link_libraries(tessera_format_wasm PRIVATE tessera_source_format)
    set_target_properties(tessera_format_wasm PROPERTIES OUTPUT_NAME tessera_format)
    target_link_options(tessera_format_wasm PRIVATE
        --no-entry
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createTesseraFormat
        -sENVIRONMENT=web,worker,node
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORTED_FUNCTIONS=["_tessera_format_edits_json","_tessera_free","_malloc","_free"]
        -sEXPORTED_RUNTIME_METHODS=["UTF8ToString","lengthBytesUTF8","stringToUTF8"])
endif ()

if (TESSERA_BUILD_TESTS)
    # Registered here for the same reason as the front-end tests: the root enables testing later,
    # and enable_testing() is idempotent, so this directory's test list is still picked up.
    enable_testing()
    add_executable(source_format_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/source_format_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/source_format_corpus_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/source_format_edits_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/source_format_css_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/source_format_cli_tests.cpp)
    target_link_libraries(source_format_tests PRIVATE tessera_fmt_core tessera_style)
    target_include_directories(source_format_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(source_format_tests PRIVATE
        TESSERA_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME source_format COMMAND source_format_tests)
endif ()
