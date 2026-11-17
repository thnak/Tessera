# SPDX-License-Identifier: MIT

# `tessera-mcp`: the Model Context Protocol stdio adapter over the harness session
# (docs/specs/designer-studio-v1.md section 8, slice D-S6; decisions/ADR-045 item 6).
#
# The adapter lives under tools/ because it is a host-side consumer, not engine code: nothing under
# src/compiler or src/harness learns that MCP exists. It links the harness (for the bounded stdio
# transport and JSON codec) and the codegen front end (for the compile-only tools), which is the
# same pair the promoted CLI links.
#
# This directory registers its own targets and ctest names, so a lane working on the tool never
# edits the shared tests/unit registry or the root CMakeLists subdirectory list beyond one line.

# The core library, so the CLI, the tests and any in-process embedding exercise the same bytes. It
# holds no `main`. It reads the open project folder (read-only, `project_files.cpp`), judged by the
# shared policy of `tessera_schema` that the Wasm module links as well.
add_library(tessera_mcp_core STATIC
    src/compile_tools.cpp
    src/in_memory_ir.cpp
    src/jsonrpc.cpp
    src/mcp_protocol.cpp
    # `set_viewport` and `snapshot` (issue #408): the agent-facing half of the studio preview. It
    # calls the SAME `scene_preview_document()` the browser module exports, so a posture set through
    # MCP reaches one layout path, not a second one.
    src/preview_tools.cpp
    # The project folder (issue #406): the host walk of the shared file policy, and the tools that
    # read it (`list_files`, folder-wide `diagnostics` and `get_ir`).
    src/project_files.cpp
    src/project_tools.cpp
    src/serve_stdio.cpp
    src/session.cpp
    src/tool_registry.cpp
    src/tool_session.cpp)

target_include_directories(tessera_mcp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The compiler's own diagnostics driver and IR writer are the implementation of the compile tools:
# `diagnose_source()` and the lowering/validation stages it wraps. `tools/codegen` is an include
# directory rather than a target because its sources are compiled into the executables that need
# them (`src/compiler/codegen/CMakeLists.txt` explains the one-path rule); the diagnostic record
# units are listed here as well so the library links without a second copy of `diagnose.cpp`.
target_sources(tessera_mcp_core PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/codegen/diagnose.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/diagnostics_json.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/diagnostic_record.cpp
    # The file-driven CLI driver: a folder-wide `diagnostics` / `get_ir` runs `cli::run` with
    # `Options::query`, the same function `tessera_codegen --diagnostics-json` runs, so the document
    # cannot drift from the CLI's (the acceptance of issue #406).
    ${PROJECT_SOURCE_DIR}/tools/codegen/regenerate.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/regenerate_assets.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/regenerate_output.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/project_identity.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/package_manifest.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/emit_library_components.cpp
    ${PROJECT_SOURCE_DIR}/tools/codegen/emit_library_components_expr.cpp)

target_include_directories(tessera_mcp_core PRIVATE ${PROJECT_SOURCE_DIR}/tools/codegen)

target_link_libraries(tessera_mcp_core PUBLIC
    tessera_codegen
    tessera_hal_metrics
    tessera_harness
    tessera_ir
    tessera_ir_ids
    tessera_lowering
    # `generate` hashes the generated files with the in-tree SHA-256 (issue #407).
    tessera_automation
    tessera_schema
    tessera_style
    # `snapshot` and `set_viewport` (issue #408): the preview driver the browser module and the
    # native preview CLI also link, so the agent's snapshot is one implementation rather than a
    # third one. The library is host-only-built and links no GPU/paint target, which keeps this
    # adapter as light as the compile tools already made it.
    tessera_scene_preview)

target_compile_features(tessera_mcp_core PUBLIC cxx_std_23)

# The protocol revision this build implements, reported by `initialize` (a build input, so two
# builds of the same commit never disagree about their identity).
target_compile_definitions(tessera_mcp_core
    PUBLIC TESSERA_MCP_VERSION="${PROJECT_VERSION}")

# The server executable. Its name is the tool's name: an MCP client is configured with a command
# line, so `tessera-mcp` must be the artifact's name, not a target alias.
add_executable(tessera_mcp
    src/main.cpp)

target_link_libraries(tessera_mcp PRIVATE tessera_mcp_core)

set_target_properties(tessera_mcp PROPERTIES OUTPUT_NAME tessera-mcp)

# Tests are registered from this directory (their sources live under tests/unit/mcp) so the shared
# tests/unit registry never has to change for this lane. This subdirectory is added before the root
# enable_testing(), so enable it here, the same way tools/icons does.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(mcp_jsonrpc_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/jsonrpc_tests.cpp)

    target_link_libraries(mcp_jsonrpc_tests PRIVATE tessera_mcp_core)

    add_test(NAME mcp_jsonrpc COMMAND mcp_jsonrpc_tests)

    # The tool surface as advertised, and the honesty rule: every declared tool is listed, and one
    # this build cannot run answers with a clean execution error rather than pretending.
    add_executable(mcp_tools_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/tools_tests.cpp)

    target_link_libraries(mcp_tools_tests PRIVATE tessera_mcp_core)

    add_test(NAME mcp_tools COMMAND mcp_tools_tests)

    # `set_viewport` and `snapshot` (issue #408): the sticky viewport, the refusal of a bad posture,
    # and the acceptance's parity claim - a posture set through MCP produces the SAME scene snapshot
    # as the driver the browser module and the native preview CLI call, byte for byte.
    add_executable(mcp_preview_tools_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/preview_tools_tests.cpp)

    target_include_directories(mcp_preview_tools_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/tools/codegen
            ${PROJECT_SOURCE_DIR}/tools/mcp/src)

    target_link_libraries(mcp_preview_tools_tests PRIVATE
        tessera_hal_metrics
        tessera_mcp_core
        tessera_scene_preview)

    add_test(NAME mcp_preview_tools COMMAND mcp_preview_tools_tests)

    # The implemented compile tools and the dispatch contract around them: which channel a failure
    # uses (protocol error for bad arguments, tool error for a call that ran and failed), and what
    # `get_ir` returns where there is no IR.
    add_executable(mcp_compile_tools_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/compile_tools_tests.cpp)

    target_link_libraries(mcp_compile_tools_tests PRIVATE tessera_mcp_core)

    add_test(NAME mcp_compile_tools COMMAND mcp_compile_tools_tests)

    # The issue's acceptance: the `diagnostics` tool's document is byte-identical to what the CLI
    # harness reports for the same input, over a corpus that must actually diagnose. The file-driven
    # driver is compiled into the test. The MCP side is `tessera_mcp_core` (which owns the diagnostics
    # writer and `diagnose.cpp`); these are the driver pieces the core does not carry, so one process
    # holds both drivers over one serializer. Same source list as `codegen_diagnostics_json_tests` in
    # src/compiler/codegen/CMakeLists.txt, minus the three units the core already provides.
    add_executable(mcp_diagnostics_parity_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/diagnostics_parity_tests.cpp)

    target_include_directories(mcp_diagnostics_parity_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/tools/codegen
            ${PROJECT_SOURCE_DIR}/tools/mcp/src)

    target_link_libraries(mcp_diagnostics_parity_tests PRIVATE
        tessera_codegen
        tessera_harness
        tessera_ir_ids
        tessera_lowering
        tessera_mcp_core
        tessera_style)

    # The scratch directory lives under the build tree, never %TEMP%: a confined sandbox denies a
    # child process writing there, and keeping it here also makes a failed run inspectable.
    add_test(NAME mcp_diagnostics_parity
        COMMAND mcp_diagnostics_parity_tests
                ${CMAKE_CURRENT_BINARY_DIR}/mcp_diagnostics_parity-work)

    # The protocol for real, over the real framing transport: the MCP lifecycle (handshake,
    # negotiation, notification, unknown method, malformed line, CRLF, EOF tail) ...
    add_executable(mcp_stdio_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/stdio_tests.cpp)

    target_link_libraries(mcp_stdio_tests PRIVATE tessera_mcp_core)

    add_test(NAME mcp_stdio COMMAND mcp_stdio_tests)

    # ... and the loop's own contract: a tools/call across the wire, idling instead of spinning,
    # the stop flag, and a rejected frame.
    add_executable(mcp_stdio_call_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/stdio_call_tests.cpp)

    target_link_libraries(mcp_stdio_call_tests PRIVATE tessera_mcp_core)

    add_test(NAME mcp_stdio_call COMMAND mcp_stdio_call_tests)

    # A real process round trip: the test spawns `tessera-mcp`, writes the handshake and a
    # `tools/call` to its stdin, and asserts the response lines. Registered only where a subprocess
    # can be spawned (the codegen CLI tests use the same guard); the in-process stdio suites above
    # cover the loop everywhere.
    add_executable(mcp_process_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/process_tests.cpp)

    target_link_libraries(mcp_process_tests PRIVATE tessera_harness)

    # The project-folder tools (issue #406): `list_files`, the folder-wide `diagnostics` / `get_ir`,
    # root resolution (explicit path, client `roots`, working directory) and the named refusals. The
    # scratch folder lives under the build tree for the same reason as the process test's. The cases
    # are split by responsibility (the project folder, then `generate` of issue #407) with their
    # fixtures in a third unit, so no one file outgrows the size rules.
    add_executable(mcp_project_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/project_tests.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/project_fixtures.cpp
        ${PROJECT_SOURCE_DIR}/tests/unit/mcp/project_generate_tests.cpp)

    target_include_directories(mcp_project_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/tools/codegen
            ${PROJECT_SOURCE_DIR}/tools/mcp/src)

    target_link_libraries(mcp_project_tests PRIVATE tessera_mcp_core tessera_schema)

    if (NOT CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS)$")
        add_test(NAME mcp_project
            COMMAND mcp_project_tests $<TARGET_FILE:tessera_mcp>
                    ${CMAKE_CURRENT_BINARY_DIR}/mcp_project-work)
    endif ()

    if (NOT CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|watchOS)$")
        add_test(NAME mcp_process
            COMMAND mcp_process_tests $<TARGET_FILE:tessera_mcp>
                    ${CMAKE_CURRENT_BINARY_DIR}/mcp_process-work)
    endif ()
endif ()
