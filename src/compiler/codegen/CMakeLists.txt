# SPDX-License-Identifier: MIT

# First C++ codegen slice (docs/design/codegen.md): IR -> native C++ source text. Codegen reads the
# IR only (AGENTS.md non-negotiable 4), so it links tessera_ir and nothing else from the project.
add_library(tessera_codegen STATIC
    src/codegen.cpp
    src/emit_component.cpp
    src/emit_expr.cpp)

target_include_directories(tessera_codegen
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(tessera_codegen PUBLIC tessera_ir)
target_compile_features(tessera_codegen PUBLIC cxx_std_23)

# Layering rule: codegen sees the IR and its own headers, never the parser, lowering or the UI.
get_target_property(_tessera_codegen_links tessera_codegen LINK_LIBRARIES)
if (NOT _tessera_codegen_links STREQUAL "tessera_ir")
    message(FATAL_ERROR "tessera_codegen must link exactly tessera_ir, found: ${_tessera_codegen_links}")
endif ()

# Generated sample compiled by the build: the golden below is written by `codegen_tests --write`
# and is generated from IR, never hand-edited. The file exists in a checkout; the guard keeps the
# first configure of a fresh worktree before the golden is written from failing.
set(_tessera_codegen_golden_dir ${CMAKE_SOURCE_DIR}/tests/golden/codegen)
if (EXISTS ${_tessera_codegen_golden_dir}/Sample.cpp)
    add_library(tessera_codegen_sample STATIC
        ${_tessera_codegen_golden_dir}/Sample.cpp)
    target_include_directories(tessera_codegen_sample PRIVATE ${_tessera_codegen_golden_dir})
    target_link_libraries(tessera_codegen_sample PRIVATE tessera_scene)
    target_compile_features(tessera_codegen_sample PUBLIC cxx_std_23)
endif ()

if (TESSERA_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)

    add_executable(codegen_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_tests.cpp)

    target_link_libraries(codegen_tests PRIVATE
        tessera_codegen
        tessera_merger
        Threads::Threads)

    target_compile_definitions(codegen_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_generate COMMAND codegen_tests)

    add_executable(codegen_layering_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_layering_tests.cpp)

    target_link_libraries(codegen_layering_tests PRIVATE tessera_codegen)

    target_compile_definitions(codegen_layering_tests PRIVATE
        TESSERA_COMPILER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/compiler")

    add_test(NAME codegen_layering COMMAND codegen_layering_tests)
endif ()
