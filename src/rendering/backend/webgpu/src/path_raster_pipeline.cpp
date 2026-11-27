// SPDX-License-Identifier: MIT

#include "tessera/rendering/webgpu/path_raster_pipeline.hpp"

#include <stdexcept>

namespace {

constexpr char kPathRasterWgsl[] =
#include "path_raster_wgsl.inc"
    ;

} // namespace

namespace tessera::rendering::webgpu {

std::string_view path_raster_shader_id(PathRasterPass pass) {
    switch (pass) {
    case PathRasterPass::Clear:
        return "path_raster_clear";
    case PathRasterPass::Stroke:
        return "path_raster_stroke";
    case PathRasterPass::Bin:
        return "path_raster_bin";
    case PathRasterPass::Fine:
        return "path_raster_fine";
    }
    throw std::invalid_argument("path_raster_shader_id: unknown pass");
}

WgslProgram path_raster_wgsl_program(PathRasterPass pass) {
    WgslProgram program;
    program.source = kPathRasterWgsl;
    switch (pass) {
    case PathRasterPass::Clear:
        program.compute_entry = "clear_main";
        break;
    case PathRasterPass::Stroke:
        program.compute_entry = "stroke_main";
        break;
    case PathRasterPass::Bin:
        program.compute_entry = "bin_main";
        break;
    case PathRasterPass::Fine:
        program.compute_entry = "fine_main";
        break;
    default:
        throw std::invalid_argument("path_raster_wgsl_program: unknown pass");
    }
    return program;
}

rhi::ComputePipelineDesc path_raster_pipeline_desc(PathRasterPass pass) {
    rhi::ComputePipelineDesc desc;
    desc.shader_id = path_raster_shader_id(pass);
    desc.storage_buffer_count = kPathRasterStorageBuffers;
    desc.push_constant_size = kPathRasterPushBytes;
    return desc;
}

path::GpuPathPipelineDescs path_raster_pipeline_descs() {
    return {path_raster_pipeline_desc(PathRasterPass::Clear),
            path_raster_pipeline_desc(PathRasterPass::Stroke),
            path_raster_pipeline_desc(PathRasterPass::Bin),
            path_raster_pipeline_desc(PathRasterPass::Fine)};
}

} // namespace tessera::rendering::webgpu
