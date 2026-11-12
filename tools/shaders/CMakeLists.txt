# SPDX-License-Identifier: MIT

# Only configured when TESSERA_SHADER_CROSS=ON (see cmake/TesseraShaderCross.cmake, ADR-049).
include(${PROJECT_SOURCE_DIR}/cmake/TesseraShaderCross.cmake)

tessera_cross_compile_shader(batched2d
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/src/rendering/backend/vulkan/shaders
    VERTEX batched2d.vert
    FRAGMENT batched2d.frag
    AUTHORED_WGSL ${PROJECT_SOURCE_DIR}/src/rendering/backend/webgpu/shaders/batched2d.wgsl)
