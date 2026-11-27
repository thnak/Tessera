// SPDX-License-Identifier: MIT

// Device half: needs register_shader, so it is built with tessera_webgpu, not the mapping library.

#include "tessera/rendering/webgpu/path_raster_pipeline.hpp"
#include "tessera/rendering/webgpu/webgpu_device.hpp"

namespace tessera::rendering::webgpu {

bool register_path_raster_shaders(rhi::Device& device) {
    for (const PathRasterPass pass : {PathRasterPass::Clear, PathRasterPass::Stroke,
                                      PathRasterPass::Bin, PathRasterPass::Fine}) {
        if (!register_shader(device, path_raster_shader_id(pass), path_raster_wgsl_program(pass))) {
            return false;
        }
    }
    return true;
}

} // namespace tessera::rendering::webgpu
