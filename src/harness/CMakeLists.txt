# SPDX-License-Identifier: MIT

# Headless automation harness: the JSON-lines protocol codec and its command/result model
# (`docs/specs/headless-automation-v1.md`). The slice owns its targets here so parallel slices
# never edit the root subdirectory list at the same time.
#
# Leaf library: strict JSON-lines framing (UTF-8, bounded line length, CRLF tolerance, EOF), the
# line transport seam with an in-process implementation, the versioned request/response envelope,
# and the snapshot/action/trace/audit command model. It links nothing from the project so the
# harness binary, tests, and any in-process embedding can share the same bytes.
add_library(tessera_harness STATIC
    artifact_sink.cpp
    command.cpp
    command_args.cpp
    command_result.cpp
    harness_handler.cpp
    in_process_transport.cpp
    json_parse.cpp
    json_string.cpp
    json_value.cpp
    json_write.cpp
    latency_audit.cpp
    line_framing.cpp
    protocol.cpp
    script.cpp
    script_runner.cpp
    script_step.cpp
    serve.cpp
    session.cpp
    stdio_transport.cpp
    utf8.cpp)

target_include_directories(tessera_harness PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_harness PUBLIC cxx_std_23)

# The engine side of the lane: the concrete AutomationHost over a live AppHost scene, the
# deterministic fixture scene the CLI runs scripts against, and the promoted `tessera` CLI. It is a
# separate target because tessera_harness deliberately links no engine target; the driver may link
# whatever the host needs.
add_library(tessera_automation_host STATIC
    cli.cpp
    headless_serve.cpp
    engine_host.cpp
    engine_host_diagnostics.cpp
    engine_host_items.cpp
    engine_host_snapshot.cpp
    fixture_scene.cpp
    manifest_loader.cpp
    manifest_scene.cpp)

target_include_directories(tessera_automation_host PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_automation_host PUBLIC
    tessera_app
    tessera_automation
    tessera_diagnostics
    tessera_harness
    tessera_input_pump
    tessera_scene
    tessera_style
    tessera_runtime)

target_compile_features(tessera_automation_host PUBLIC cxx_std_23)

# `tessera` CLI. Promoted out of EXCLUDE_FROM_ALL now that `--script` runs: the documented
# invocation must be buildable in the default configuration. The alias/install name stays a
# root-CMakeLists concern (Lead-owned).
add_executable(tessera_harness_cli
    cli_main.cpp)

target_link_libraries(tessera_harness_cli PRIVATE tessera_automation_host)
