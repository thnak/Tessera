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
