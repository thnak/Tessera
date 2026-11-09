# SPDX-License-Identifier: MIT

# First C++ codegen slice (docs/design/codegen.md): IR -> native C++ source text. Codegen reads the
# IR only (AGENTS.md non-negotiable 4), so it links tessera_ir and nothing else from the project.
add_library(tessera_codegen STATIC
    src/codegen.cpp
    src/emit_component.cpp
    src/emit_events.cpp
    src/emit_expr.cpp
    src/emit_header.cpp
    src/emit_lifecycle.cpp)

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

# The CLI driver is the only place that reads JSX and calls the front end; it is a separate target
# so the tessera_codegen library keeps its IR-only dependency (docs/guides/developer-workflow.md).
add_executable(tessera_codegen_cli
    ${CMAKE_SOURCE_DIR}/tools/codegen/main.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate.cpp)

target_include_directories(tessera_codegen_cli PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
target_link_libraries(tessera_codegen_cli PRIVATE
    tessera_codegen
    tessera_lowering
    tessera_merger)
set_target_properties(tessera_codegen_cli PROPERTIES OUTPUT_NAME tessera_codegen)

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

# Big Step C sample: seed the committed developer file (protected regions) into the build dir and
# regenerate over it with the CLI; the merged output is what the build compiles.
set(_tessera_codegen_demo_dir ${CMAKE_CURRENT_BINARY_DIR}/samples/compiler-demo)
set(_tessera_codegen_demo_src ${CMAKE_SOURCE_DIR}/samples/compiler-demo)
set(_tessera_codegen_demo_seed ${CMAKE_SOURCE_DIR}/tests/golden/codegen/demo/Demo.cpp)
add_custom_command(
    OUTPUT ${_tessera_codegen_demo_dir}/Demo.cpp ${_tessera_codegen_demo_dir}/Demo.hpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_tessera_codegen_demo_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_tessera_codegen_demo_seed} ${_tessera_codegen_demo_dir}/Demo.cpp
    COMMAND $<TARGET_FILE:tessera_codegen_cli> --write
            --out ${_tessera_codegen_demo_dir} --namespace demo
            ${_tessera_codegen_demo_src}/Demo.tsx
    DEPENDS tessera_codegen_cli
            ${_tessera_codegen_demo_src}/Demo.tsx
            ${_tessera_codegen_demo_seed}
    COMMENT "Regenerating the compiler demo screen"
    VERBATIM)

add_library(tessera_compiler_demo STATIC ${_tessera_codegen_demo_dir}/Demo.cpp)
target_include_directories(tessera_compiler_demo PRIVATE ${_tessera_codegen_demo_dir})
target_link_libraries(tessera_compiler_demo PRIVATE tessera_scene)

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

    # End-to-end proof: build the generated sample and inspect the attached metadata, children,
    # focusability and text through the real scene API (no GPU).
    add_executable(codegen_runtime_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_runtime_tests.cpp)

    target_include_directories(codegen_runtime_tests PRIVATE ${_tessera_codegen_golden_dir})
    target_link_libraries(codegen_runtime_tests PRIVATE
        tessera_codegen_sample
        tessera_scene)

    add_test(NAME codegen_runtime COMMAND codegen_runtime_tests)

    add_executable(codegen_cli_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_cli_tests.cpp)

    target_link_libraries(codegen_cli_tests PRIVATE tessera_codegen)

    add_test(NAME codegen_cli
        COMMAND codegen_cli_tests ${_tessera_codegen_demo_src}
                ${CMAKE_SOURCE_DIR}/tests/golden/codegen/demo $<TARGET_FILE:tessera_codegen_cli>)

    add_test(NAME sample_compile
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target tessera_compiler_demo)
endif ()
