# SPDX-License-Identifier: MIT

# Automation identity and snapshot model (`docs/specs/automation-ids-v1.md`). The slice owns its
# targets here so parallel slices never edit the root subdirectory list at the same time.
#
# Leaf library: in-tree SHA-256, salted hash64 UiIds with explicit-automationId diagnostics, the
# preorder snapshot builder, and its canonical JSON serializer. It links nothing from the project
# so the compiler, codegen, the runtime and headless harnesses can consume the same numbers.
add_library(tessera_automation STATIC
    id.cpp
    sha256.cpp
    snapshot.cpp
    snapshot_json.cpp)

target_include_directories(tessera_automation PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_compile_features(tessera_automation PUBLIC cxx_std_23)

# The scene -> snapshot walk of docs/specs/automation-ids-v1.md section 5.2 (issue #166). It reads a
# live `ui::Node` tree, so it is a separate target: `tessera_automation` above stays a leaf that
# links nothing, which is what lets the compiler, the id assignment and the codegen fixtures consume
# the builder and serializer without the scene. The two callers are the headless harness
# (src/harness/engine_host_snapshot.cpp) and the design-time preview's in-memory path
# (tools/codegen/scene_preview.cpp); they share this one walk so the parity gate compares one
# implementation of the artifact, and the walk itself needs no filesystem, GPU, RTTI or clock.
add_library(tessera_automation_scene STATIC
    scene_snapshot.cpp)

target_include_directories(tessera_automation_scene PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(tessera_automation_scene PUBLIC
    tessera_automation
    tessera_scene)

target_compile_features(tessera_automation_scene PUBLIC cxx_std_23)

# The walk's contract (issue #166): preorder emission with parent indices, derived structural paths,
# the annotation override, hidden-subtree omission, both caps, and the canonical document bytes. The
# harness's own suite covers the same walk through `EngineAutomationHost`; this one pins it directly,
# so a change to the shared code fails here first. Registered in this module (not
# tests/unit/CMakeLists.txt) so a lane adding it never edits that shared file.
if (TESSERA_BUILD_TESTS)
    enable_testing()

    add_executable(scene_snapshot_tests
        ${PROJECT_SOURCE_DIR}/tests/unit/scene_snapshot_tests.cpp)

    target_link_libraries(scene_snapshot_tests PRIVATE
        tessera_automation_scene
        tessera_harness)

    add_test(NAME scene_snapshot COMMAND scene_snapshot_tests)
endif()


