# SPDX-License-Identifier: MIT

# Build-time shader manifest validator CLI (docs/specs/shader-manifest-v1.md sections 5 and 9).
# The Vulkan and TESSERA_SHADER_CROSS shader rules run it before glslc; the manifest path is set
# by the top-level CMakeLists.txt so both rules use the same file.
#
# The tool runs on the BUILD HOST, never on the target: a cross-compiling build (Android, iOS) would
# otherwise produce a target binary the host cannot execute, and every shader rule would fail with
# "'tessera_shader_manifest_check' is not recognized". `TESSERA_SHADER_MANIFEST_CHECK` is therefore
# resolved by the caller (the top-level CMakeLists.txt) to either this target's executable or a
# host-built one; the rules use that variable, not the target name.
add_executable(tessera_shader_manifest_check manifest_check.cpp)

target_link_libraries(tessera_shader_manifest_check PRIVATE tessera_schema)

target_compile_features(tessera_shader_manifest_check PRIVATE cxx_std_23)
