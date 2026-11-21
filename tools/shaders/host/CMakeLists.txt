# SPDX-License-Identifier: MIT

# Host build of the shader manifest checker (docs/specs/shader-manifest-v1.md sections 5 and 9).
#
# A cross-compiling build (Android, iOS) cannot run a target executable at build time, so the shader
# rules need this tool built for the BUILD HOST. The top-level CMakeLists.txt drives this project
# through ExternalProject_Add when CMAKE_CROSSCOMPILING is set, and points
# TESSERA_SHADER_MANIFEST_CHECK at the resulting binary.
#
# It is a separate project on purpose: it must not inherit the target toolchain file, the target
# sysroot, or any of the parent's cache. Only `tessera_schema` and the tool's one source are built,
# and `tessera_schema` is a leaf library (it links nothing), so this stays a small, fast build.
cmake_minimum_required(VERSION 3.24)

project(TesseraShaderManifestCheckHost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The parent passes the source root; the schema target is defined by its own CMakeLists.txt.
if (NOT TESSERA_HOST_SOURCE_DIR)
    message(FATAL_ERROR "TESSERA_HOST_SOURCE_DIR must name the Tessera source root")
endif ()

add_subdirectory("${TESSERA_HOST_SOURCE_DIR}/src/schema" schema)

add_executable(tessera_shader_manifest_check
    "${TESSERA_HOST_SOURCE_DIR}/tools/shaders/manifest_check.cpp")

target_link_libraries(tessera_shader_manifest_check PRIVATE tessera_schema)

target_compile_features(tessera_shader_manifest_check PRIVATE cxx_std_23)
