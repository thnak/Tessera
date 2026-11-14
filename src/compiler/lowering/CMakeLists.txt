# SPDX-License-Identifier: MIT

# Tier 2 lowering (docs/design/jsx-ir-lowering.md section 9). It reads the AST and the analysis
# result and builds the IR, so its core links the front end, the analysis pass, the shared schema
# table and the IR, and nothing else from the project.
#
# Issue #70 splits the library component registry out of the core so the build can generate it from
# `packages/tessera-ui/{Field,Form}.tsx` without a target cycle: the generator links the core, the generated
# translation unit links the generator, and the public `tessera_lowering` combines both.
set(_tessera_lowering_core_sources
    src/binding_lowering.cpp
    src/call_lowering.cpp
    src/component_lowering.cpp
    src/component_names.cpp
    src/control_lowering.cpp
    src/callback_forward.cpp
    src/event_lowering.cpp
    src/expression_lowering.cpp
    src/handler_actions.cpp
    src/hook_lowering.cpp
    src/hook_lowering_support.cpp
    src/hook_lookups.cpp
    src/hook_type.cpp
    src/literal_lowering.cpp
    src/module_lowering.cpp
    src/node_attributes.cpp
    src/node_children.cpp
    src/node_children_items.cpp
    src/node_lowering.cpp
    src/project_path.cpp
    src/prop_records.cpp
    src/reference_lowering.cpp
    src/schema_bridge.cpp
    src/source_span.cpp
    src/style_lowering.cpp
    src/template_lowering.cpp
    src/type_alias_table.cpp)

add_library(tessera_lowering_core STATIC ${_tessera_lowering_core_sources})

target_include_directories(tessera_lowering_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(tessera_lowering_core PUBLIC
    tessera_jsx_frontend
    tessera_ir
    tessera_react_rules
    tessera_schema)
target_compile_features(tessera_lowering_core PUBLIC cxx_std_23)

# Layering rule of docs/design/jsx-ir-lowering.md section 6: lowering is the only layer that sees
# the AST, the analysis, the shared schema and the IR. Keep the core dependency list closed.
get_target_property(_tessera_lowering_core_links tessera_lowering_core LINK_LIBRARIES)
if (NOT _tessera_lowering_core_links STREQUAL
    "tessera_jsx_frontend;tessera_ir;tessera_react_rules;tessera_schema")
    message(FATAL_ERROR
        "tessera_lowering_core must link exactly tessera_jsx_frontend, tessera_ir, "
        "tessera_react_rules and tessera_schema, found: ${_tessera_lowering_core_links}")
endif ()

# The build-time bootstrap registry: the generator lowers the library TSX before the generated
# registry exists, so its executable links this empty registry instead of the data it produces.
add_library(tessera_lowering_bootstrap STATIC src/library_components_bootstrap.cpp)
target_include_directories(tessera_lowering_bootstrap
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tessera_lowering_bootstrap PUBLIC tessera_ir)
target_compile_features(tessera_lowering_bootstrap PUBLIC cxx_std_23)

# The TSX sources are the single source of truth (issue #70): regenerate the IR translation unit
# whenever either file changes. Host builds compile the freshly generated unit and verify that the
# committed copy (which cross-compiling builds consume, because they cannot execute the generator)
# is current. Stale committed data fails the host build rather than silently shipping to mobile.
set(_tessera_library_tsx_dir ${CMAKE_SOURCE_DIR}/packages/tessera-ui)
set(_tessera_library_tsx ${_tessera_library_tsx_dir}/Field.tsx ${_tessera_library_tsx_dir}/Form.tsx)
set(_tessera_library_committed_cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/library_components_generated.cpp)

if (NOT CMAKE_CROSSCOMPILING)
    # The generator executable: the ordinary codegen CLI sources plus the C++ IR writer, linked
    # against the lowering core only. It is deliberately not `tessera_lowering` (which owns the
    # generated data) so the generated source can depend on the generator without a build cycle.
    # `tessera_style` comes with the CLI: it compiles CSS into the blobs codegen embeds (issue #81),
    # and this executable links the same driver sources as `tessera_codegen_cli`.
    add_executable(tessera_library_ir_gen
        ${CMAKE_SOURCE_DIR}/tools/codegen/main.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/regenerate.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/project_identity.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/package_manifest.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/emit_library_components.cpp
        ${CMAKE_SOURCE_DIR}/tools/codegen/emit_library_components_expr.cpp)
    target_include_directories(tessera_library_ir_gen PRIVATE ${CMAKE_SOURCE_DIR}/tools/codegen)
    target_link_libraries(tessera_library_ir_gen PRIVATE
        tessera_codegen
        tessera_harness
        tessera_ir_ids
        tessera_lowering_core
        tessera_lowering_bootstrap
        tessera_merger
        tessera_style)

    set(_tessera_library_generated_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
    set(_tessera_library_generated_cpp
        ${_tessera_library_generated_dir}/library_components_generated.cpp)
    add_custom_command(
        OUTPUT ${_tessera_library_generated_cpp}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_tessera_library_generated_dir}
        COMMAND $<TARGET_FILE:tessera_library_ir_gen>
                --emit-library-cpp ${_tessera_library_generated_cpp}
                packages/tessera-ui/Field.tsx packages/tessera-ui/Form.tsx
        DEPENDS
            tessera_library_ir_gen
            ${_tessera_library_tsx}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/library_components_generated.hpp
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating the library component IR from packages/tessera-ui/{Field,Form}.tsx"
        VERBATIM)
else ()
    # A cross build cannot execute the host generator; it consumes the committed copy that the host
    # build verifies. There is no hand-built fallback.
    set(_tessera_library_generated_cpp ${_tessera_library_committed_cpp})
endif ()

# The public contract (`library_components.cpp`) plus the generated IR data. The contract object
# and the generated object live in one archive, so the contract's calls resolve inside it.
add_library(tessera_library_components STATIC
    src/library_components.cpp
    ${_tessera_library_generated_cpp})
target_include_directories(tessera_library_components
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tessera_library_components PUBLIC tessera_ir)
target_compile_features(tessera_library_components PUBLIC cxx_std_23)

# Host builds compare the committed copy with the generated one: the mobile lanes compile the
# committed file, so it must never be stale.
if (NOT CMAKE_CROSSCOMPILING)
    add_custom_target(tessera_library_components_check
        COMMAND ${CMAKE_COMMAND} -E compare_files
                ${_tessera_library_committed_cpp} ${_tessera_library_generated_cpp}
        DEPENDS ${_tessera_library_generated_cpp} ${_tessera_library_committed_cpp}
        COMMENT "Checking committed library component IR against packages/tessera-ui/{Field,Form}.tsx"
        VERBATIM)
    add_dependencies(tessera_library_components tessera_library_components_check)
endif ()

# The public lowering target: core pipeline + generated library component registry.
add_library(tessera_lowering INTERFACE)
target_link_libraries(tessera_lowering INTERFACE
    tessera_lowering_core
    tessera_library_components)
