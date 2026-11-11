# SPDX-License-Identifier: MIT

# Numeric identity for the IR (docs/specs/automation-ids-v1.md sections 2-3): componentId and uiId
# from the project salt, the source path and the structural node path. It uses the hashing of
# `tessera_automation` and the IR types, and nothing else of the project.
add_library(tessera_ir_ids STATIC
    src/assign_identity.cpp)

target_include_directories(tessera_ir_ids
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

target_compile_features(tessera_ir_ids PUBLIC cxx_std_23)

target_link_libraries(tessera_ir_ids PUBLIC tessera_ir PRIVATE tessera_automation)
