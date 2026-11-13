# SPDX-License-Identifier: MIT

# Host-buildable, WebGPU-API-free half: descriptor mapping tables and the WGSL shader registry.
# Built everywhere (cheap, pure) so its unit tests run in the normal host ctest.
set(TESSERA_WGSL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(TESSERA_WGSL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)

# ADR-011: WGSL is authored directly, no runtime translation. Embed it as a raw string at configure
# time (re-run automatically when the .wgsl changes) so production loads no shader files.
function(tessera_embed_wgsl shader)
    get_filename_component(name ${shader} NAME_WE)
    file(READ ${TESSERA_WGSL_DIR}/${shader} contents)
    file(WRITE ${TESSERA_WGSL_GEN_DIR}/${name}_wgsl.inc "R\"WGSL(${contents})WGSL\"")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TESSERA_WGSL_DIR}/${shader})
endfunction()

tessera_embed_wgsl(batched2d.wgsl)
# Glass two-pass Gaussian twin (issue #244); validated by naga and the Wasm lane.
tessera_embed_wgsl(glass_blur.wgsl)

add_library(tessera_webgpu_mapping STATIC
    src/webgpu_mapping.cpp
    src/shader_registry.cpp)

target_include_directories(tessera_webgpu_mapping
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${TESSERA_WGSL_GEN_DIR})
target_link_libraries(tessera_webgpu_mapping PUBLIC tessera_rhi)
target_compile_features(tessera_webgpu_mapping PUBLIC cxx_std_23)

# The device half needs <webgpu/webgpu.h>: Emscripten provides it (emdawnwebgpu port), or a native
# Dawn/wgpu-native build when TESSERA_BUILD_WEBGPU is ON. Never part of a default host build.
if (EMSCRIPTEN OR TESSERA_BUILD_WEBGPU)
    add_library(tessera_webgpu STATIC
        src/webgpu_device.cpp
        src/webgpu_resources.cpp
        src/webgpu_pipeline.cpp
        src/webgpu_command_list.cpp)
    target_link_libraries(tessera_webgpu PUBLIC tessera_webgpu_mapping)
    if (EMSCRIPTEN)
        # Dawn's emdawnwebgpu port supplies webgpu/webgpu.h and the browser bindings. The port
        # revision is the one pinned by the emsdk release in tools/emsdk-version.txt.
        target_compile_options(tessera_webgpu PUBLIC "--use-port=emdawnwebgpu")
        target_link_options(tessera_webgpu PUBLIC "--use-port=emdawnwebgpu")

        # Link-and-load smoke run under Node (no WebGPU there; see the source comment).
        add_executable(webgpu_wasm_smoke smoke/webgpu_wasm_smoke.cpp)
        target_link_libraries(webgpu_wasm_smoke PRIVATE tessera_webgpu)
        target_link_options(webgpu_wasm_smoke PRIVATE "-sENVIRONMENT=node")
    endif()
endif()
