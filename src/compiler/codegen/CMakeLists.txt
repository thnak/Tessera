# SPDX-License-Identifier: MIT

# First C++ codegen slice (docs/design/codegen.md): IR -> native C++ source text. Codegen reads the
# IR only (AGENTS.md non-negotiable 4), so it links tessera_ir and nothing else from the project.
add_library(tessera_codegen STATIC
    src/codegen.cpp
    src/emit_component.cpp
    src/emit_composition.cpp
    src/emit_conditional.cpp
    src/emit_controlled.cpp
    src/emit_dynamic_header.cpp
    src/emit_dynamic_plan.cpp
    src/emit_events.cpp
    src/emit_expr.cpp
    src/emit_forms.cpp
    src/emit_layout_observe.cpp
    src/emit_schedule.cpp
    src/emit_snapshot.cpp
    src/form_binding.cpp
    src/emit_header.cpp
    src/emit_lifecycle.cpp
    src/emit_list.cpp
    src/emit_node.cpp
    src/emit_props.cpp
    src/emit_virtual.cpp
    src/emit_virtual_geometry.cpp
    src/emit_virtual_scroll.cpp
    src/interpolation.cpp
    src/value_ops.cpp)

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
    ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/project_identity.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/emit_library_components.cpp
    ${CMAKE_SOURCE_DIR}/tools/codegen/emit_library_components_expr.cpp)

target_include_directories(tessera_codegen_cli PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
target_link_libraries(tessera_codegen_cli PRIVATE
    tessera_codegen
    tessera_harness
    tessera_ir_ids
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
    target_link_libraries(tessera_codegen_sample PRIVATE tessera_scene tessera_text_edit)
    target_compile_features(tessera_codegen_sample PUBLIC cxx_std_23)
endif ()

# Conditional + keyed-list fixture (generated from the hand-built IR in codegen_dynamic_tests).
# Same deliberate golden path: `codegen_dynamic_tests --write` owns Dynamic.cpp/hpp.
if (EXISTS ${_tessera_codegen_golden_dir}/Dynamic.cpp)
    add_library(tessera_codegen_dynamic_sample STATIC
        ${_tessera_codegen_golden_dir}/Dynamic.cpp)
    target_include_directories(tessera_codegen_dynamic_sample PRIVATE ${_tessera_codegen_golden_dir})
    target_link_libraries(tessera_codegen_dynamic_sample PRIVATE tessera_scene)
    target_compile_features(tessera_codegen_dynamic_sample PUBLIC cxx_std_23)
endif ()

# Composition + static children-slot fixture (generated from the hand-built IR in
# codegen_composition_tests). Same deliberate golden path: `codegen_composition_tests --write` owns
# Card.* and CardPage.*.
if (EXISTS ${_tessera_codegen_golden_dir}/CardPage.cpp AND EXISTS ${_tessera_codegen_golden_dir}/Card.cpp)
    add_library(tessera_codegen_composition_sample STATIC
        ${_tessera_codegen_golden_dir}/Card.cpp
        ${_tessera_codegen_golden_dir}/CardPage.cpp)
    target_include_directories(tessera_codegen_composition_sample
        PRIVATE ${_tessera_codegen_golden_dir})
    target_link_libraries(tessera_codegen_composition_sample PRIVATE tessera_scene)
    target_compile_features(tessera_codegen_composition_sample PUBLIC cxx_std_23)
endif ()

# onSizeChange fixture (ADR-041 slice 5), generated from the hand-built IR in
# codegen_size_change_tests. Same deliberate golden path: `codegen_size_change_tests --write` owns
# Watcher.* and WatchPage.*. It links tessera_layout for LayoutObservation.
if (EXISTS ${_tessera_codegen_golden_dir}/Watcher.cpp AND EXISTS ${_tessera_codegen_golden_dir}/WatchPage.cpp)
    add_library(tessera_codegen_size_watch_sample STATIC
        ${_tessera_codegen_golden_dir}/Watcher.cpp
        ${_tessera_codegen_golden_dir}/WatchPage.cpp)
    target_include_directories(tessera_codegen_size_watch_sample
        PRIVATE ${_tessera_codegen_golden_dir})
    target_link_libraries(tessera_codegen_size_watch_sample PUBLIC tessera_scene tessera_layout)
    target_compile_features(tessera_codegen_size_watch_sample PUBLIC cxx_std_23)
endif ()

# Big Step C sample: seed the committed developer file (protected regions) into the build dir and
# regenerate over it with the CLI; the merged output is what the build compiles.
#
# The regeneration runs the target-built `tessera_codegen_cli` ON THE HOST at build time, so it is
# host-only: a cross-compiling build (Android/iOS) would try to execute a target binary. Generators
# run on the host, never the target (same rule as the embedded-asset generator); mobile builds skip
# the demo entirely and the host platforms cover it.
if (NOT CMAKE_CROSSCOMPILING)
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
target_link_libraries(tessera_compiler_demo PRIVATE tessera_scene tessera_text_edit)
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

    # End-to-end proof: build the generated sample and inspect the attached metadata, children,
    # focusability and text through the real scene API (no GPU).
    add_executable(codegen_runtime_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_runtime_tests.cpp)

    target_include_directories(codegen_runtime_tests PRIVATE ${_tessera_codegen_golden_dir})
    target_link_libraries(codegen_runtime_tests PRIVATE
        tessera_codegen_sample
        tessera_scene
        tessera_style)

    add_test(NAME codegen_runtime COMMAND codegen_runtime_tests)

    # Scheduled updates (issue #5): setters schedule, the flush runs update() once.
    add_executable(codegen_schedule_runtime_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_schedule_runtime_tests.cpp)

    target_include_directories(codegen_schedule_runtime_tests PRIVATE ${_tessera_codegen_golden_dir})
    target_link_libraries(codegen_schedule_runtime_tests PRIVATE
        tessera_codegen_sample
        tessera_scene
        tessera_runtime)

    add_test(NAME codegen_schedule_runtime COMMAND codegen_schedule_runtime_tests)

    # Conditional + keyed-list codegen: goldens from hand-built IR (the `.tsx` front end cannot
    # author a list type yet) and a runtime test over the compiled generated component.
    add_executable(codegen_dynamic_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_dynamic_tests.cpp)

    target_link_libraries(codegen_dynamic_tests PRIVATE
        tessera_codegen
        tessera_merger)

    target_compile_definitions(codegen_dynamic_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_dynamic COMMAND codegen_dynamic_tests)

    if (TARGET tessera_codegen_dynamic_sample)
        add_executable(codegen_dynamic_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_dynamic_runtime_tests.cpp)

        target_include_directories(codegen_dynamic_runtime_tests PRIVATE
            ${_tessera_codegen_golden_dir})
        target_link_libraries(codegen_dynamic_runtime_tests PRIVATE
            tessera_codegen_dynamic_sample
            tessera_scene)

        add_test(NAME codegen_dynamic_runtime COMMAND codegen_dynamic_runtime_tests)
    endif ()

    # Component composition and the static `children` slot: goldens from hand-built IR plus a
    # runtime test over the compiled generated components (a nested instance keeps its own state).
    add_executable(codegen_composition_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_composition_tests.cpp)

    target_link_libraries(codegen_composition_tests PRIVATE
        tessera_codegen
        tessera_merger)

    target_compile_definitions(codegen_composition_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_composition COMMAND codegen_composition_tests)

    if (TARGET tessera_codegen_composition_sample)
        add_executable(codegen_composition_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_composition_runtime_tests.cpp)

        target_include_directories(codegen_composition_runtime_tests PRIVATE
            ${_tessera_codegen_golden_dir})
        target_link_libraries(codegen_composition_runtime_tests PRIVATE
            tessera_codegen_composition_sample
            tessera_scene)

        add_test(NAME codegen_composition_runtime COMMAND codegen_composition_runtime_tests)
    endif ()

    add_executable(codegen_size_change_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_size_change_tests.cpp)

    target_link_libraries(codegen_size_change_tests PRIVATE
        tessera_codegen
        tessera_merger)

    target_compile_definitions(codegen_size_change_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_size_change COMMAND codegen_size_change_tests)

    if (TARGET tessera_codegen_size_watch_sample)
        add_executable(codegen_size_change_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_size_change_runtime_tests.cpp)

        target_include_directories(codegen_size_change_runtime_tests PRIVATE
            ${_tessera_codegen_golden_dir})
        target_link_libraries(codegen_size_change_runtime_tests PRIVATE
            tessera_codegen_size_watch_sample
            tessera_scene
            tessera_layout)

        add_test(NAME codegen_size_change_runtime COMMAND codegen_size_change_runtime_tests)
    endif ()

    # VirtualList/VirtualGrid item templates: goldens from hand-built IR (the `.tsx` front end
    # cannot author a virtual collection yet) and an integration test that scripts a scroll through
    # the `tessera_virtual` geometry and checks the realized item range.
    add_executable(codegen_virtual_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_virtual_tests.cpp)

    target_link_libraries(codegen_virtual_tests PRIVATE
        tessera_codegen
        tessera_merger)

    target_compile_definitions(codegen_virtual_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME codegen_virtual COMMAND codegen_virtual_tests)

    # Dynamic virtual geometry: the re-seed path, the measurement-invalidation rule and the
    # literal-only structural props (one virtual collection geometry, not the whole template).
    add_executable(codegen_virtual_geometry_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_virtual_geometry_tests.cpp)

    target_link_libraries(codegen_virtual_geometry_tests PRIVATE
        tessera_codegen
        tessera_merger)

    add_test(NAME codegen_virtual_geometry COMMAND codegen_virtual_geometry_tests)

    # The sample library owns the committed golden: `codegen_virtual_tests --write` regenerates
    # tests/golden/codegen/VirtualDemo.*; the guard keeps a fresh worktree configuring before the
    # golden exists.
    if (EXISTS ${_tessera_codegen_golden_dir}/VirtualDemo.cpp)
        add_library(tessera_codegen_virtual_sample STATIC
            ${_tessera_codegen_golden_dir}/VirtualDemo.cpp)
        target_include_directories(tessera_codegen_virtual_sample
            PRIVATE ${_tessera_codegen_golden_dir})
        target_link_libraries(tessera_codegen_virtual_sample PRIVATE
            tessera_scene
            tessera_virtual)
        target_compile_features(tessera_codegen_virtual_sample PUBLIC cxx_std_23)

        add_executable(codegen_virtual_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_virtual_runtime_tests.cpp)

        target_include_directories(codegen_virtual_runtime_tests
            PRIVATE ${_tessera_codegen_golden_dir})
        target_link_libraries(codegen_virtual_runtime_tests PRIVATE
            tessera_codegen_virtual_sample
            tessera_scene
            tessera_virtual)

        add_test(NAME codegen_virtual_runtime COMMAND codegen_virtual_runtime_tests)

        # The generated scroll entry point: geometry ownership, overscanned window,
        # measurement feedback/re-anchor, and the edge/throttle `onEndReached` contract.
        add_executable(codegen_virtual_scroll_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_virtual_scroll_tests.cpp)

        target_include_directories(codegen_virtual_scroll_tests
            PRIVATE ${_tessera_codegen_golden_dir})
        target_link_libraries(codegen_virtual_scroll_tests PRIVATE
            tessera_codegen_virtual_sample
            tessera_scene
            tessera_virtual)

        add_test(NAME codegen_virtual_scroll COMMAND codegen_virtual_scroll_tests)

        # Dynamic geometry at runtime: the state/prop-driven re-seed path (once per change, not per
        # frame), the measurement-invalidation rule, the re-anchor and its clamp, and the untouched
        # `onEndReached` edge/throttle state.
        add_executable(codegen_virtual_geometry_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/codegen_virtual_geometry_runtime_tests.cpp)

        target_include_directories(codegen_virtual_geometry_runtime_tests
            PRIVATE ${_tessera_codegen_golden_dir})
        target_link_libraries(codegen_virtual_geometry_runtime_tests PRIVATE
            tessera_codegen_virtual_sample
            tessera_scene
            tessera_virtual)

        add_test(NAME codegen_virtual_geometry_runtime
            COMMAND codegen_virtual_geometry_runtime_tests)
    endif ()

    # `Field` as a library component on the ordinary component path: a framework import names the
    # built-in component, lowering emits an ordinary `Component` node with the documented prop
    # records, and the static Children slot carries the caller's widget.
    add_executable(forms_lowering_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/forms_lowering_tests.cpp)

    target_link_libraries(forms_lowering_tests PRIVATE
        tessera_lowering
        tessera_jsx_frontend
        tessera_react_rules
        tessera_ir)

    add_test(NAME forms_lowering COMMAND forms_lowering_tests)

    # `Field` codegen: the real built-in Field IR declaration plus a form with two instances, the prop
    # matrix (value and callback props), the static Children slot, the golden and determinism.
    add_executable(forms_codegen_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/forms_codegen_tests.cpp)

    target_link_libraries(forms_codegen_tests PRIVATE
        tessera_codegen
        tessera_lowering
        tessera_merger)

    target_compile_definitions(forms_codegen_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME forms_codegen COMMAND forms_codegen_tests)

    # The golden sample owns the committed files: `forms_codegen_tests --write` regenerates
    # tests/golden/codegen/Field.* and FormPage.*; the guard keeps a fresh worktree configuring
    # before the goldens exist.
    if (EXISTS ${_tessera_codegen_golden_dir}/Field.cpp AND EXISTS
        ${_tessera_codegen_golden_dir}/Form.cpp AND EXISTS
        ${_tessera_codegen_golden_dir}/FieldPage.cpp AND EXISTS
        ${_tessera_codegen_golden_dir}/FormPage.cpp)
        add_library(tessera_codegen_forms_sample STATIC
            ${_tessera_codegen_golden_dir}/Field.cpp
            ${_tessera_codegen_golden_dir}/Form.cpp
            ${_tessera_codegen_golden_dir}/FieldPage.cpp
            ${_tessera_codegen_golden_dir}/FormPage.cpp)
        target_include_directories(tessera_codegen_forms_sample
            PRIVATE ${_tessera_codegen_golden_dir})
        target_link_libraries(tessera_codegen_forms_sample PRIVATE tessera_scene tessera_forms)
        target_compile_features(tessera_codegen_forms_sample PUBLIC cxx_std_23)

        add_executable(forms_codegen_runtime_tests
            ${CMAKE_SOURCE_DIR}/tests/unit/forms_codegen_runtime_tests.cpp)

        target_include_directories(forms_codegen_runtime_tests
            PRIVATE ${_tessera_codegen_golden_dir})
        target_link_libraries(forms_codegen_runtime_tests PRIVATE
            tessera_codegen_forms_sample
            tessera_forms
            tessera_runtime
            tessera_scene)

        add_test(NAME forms_codegen_runtime COMMAND forms_codegen_runtime_tests)
    endif ()

    # `Form` binding rejection matrix: explicit controller-owned Field props, malformed seeds,
    # duplicate names, unsupported validators and nested Forms are rejected loudly.
    add_executable(forms_form_codegen_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/forms_form_codegen_tests.cpp)

    target_link_libraries(forms_form_codegen_tests PRIVATE
        tessera_codegen
        tessera_lowering)

    target_compile_definitions(forms_form_codegen_tests PRIVATE
        TESSERA_CODEGEN_GOLDEN_DIR="${_tessera_codegen_golden_dir}")

    add_test(NAME forms_form_codegen COMMAND forms_form_codegen_tests)

    add_executable(codegen_cli_tests
        ${CMAKE_SOURCE_DIR}/tests/unit/codegen_cli_tests.cpp)

    target_link_libraries(codegen_cli_tests PRIVATE tessera_codegen)

    # iOS/tvOS/watchOS do not expose system(); the CLI tests skip there (printed reason).
    if (CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS)$")
        target_compile_definitions(codegen_cli_tests PRIVATE TESSERA_NO_SUBPROCESS=1)
    endif ()

    add_test(NAME codegen_cli
        COMMAND codegen_cli_tests ${_tessera_codegen_demo_src}
                ${CMAKE_SOURCE_DIR}/tests/golden/codegen/demo $<TARGET_FILE:tessera_codegen_cli>)

    # The demo target only exists on host builds (see the cross-compiling guard above).
    if (NOT CMAKE_CROSSCOMPILING)
        add_test(NAME sample_compile
            COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target tessera_compiler_demo)
    endif ()
endif ()
