# SPDX-License-Identifier: MIT

# Build-time shader manifest validator CLI (docs/specs/shader-manifest-v1.md sections 5 and 9).
# The Vulkan and TESSERA_SHADER_CROSS shader rules run it before glslc; the manifest path is set
# by the top-level CMakeLists.txt so both rules use the same file.
add_executable(tessera_shader_manifest_check manifest_check.cpp)

target_link_libraries(tessera_shader_manifest_check PRIVATE tessera_schema)

target_compile_features(tessera_shader_manifest_check PRIVATE cxx_std_23)
