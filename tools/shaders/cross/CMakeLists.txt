# SPDX-License-Identifier: MIT

# Opt-in build-time shader cross-compilation (ADR-049, TESSERA_SHADER_CROSS).
#
# This stays in its own subdirectory so its ctest checks register after enable_testing(); the
# manifest checker target it runs before glslc is defined earlier in tools/shaders.
include(${PROJECT_SOURCE_DIR}/cmake/TesseraShaderCross.cmake)

tessera_cross_compile_shader(batched2d
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/shaders
    VERTEX batched2d.vert
    FRAGMENT batched2d.frag
    AUTHORED_WGSL ${PROJECT_SOURCE_DIR}/src/rendering/backend/webgpu/shaders/batched2d.wgsl)

tessera_cross_compile_shader(glass_blur
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/shaders
    VERTEX fullscreen.vert
    FRAGMENT glass_blur.frag
    AUTHORED_WGSL ${PROJECT_SOURCE_DIR}/src/rendering/backend/webgpu/shaders/glass_blur.wgsl)
