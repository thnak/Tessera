// SPDX-License-Identifier: MIT
//
// Compute pipeline identities of the GPU path rasterizer (ADR-071): the four passes compiled from
// shaders/path_raster.comp, see include/tessera/rendering/vulkan/path_raster_pipeline.hpp.

#include "tessera/rendering/vulkan/path_raster_pipeline.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace {

alignas(4) const std::uint32_t kClearSpv[] =
#include "path_raster_clear_spv.h"
    ;
alignas(4) const std::uint32_t kStrokeSpv[] =
#include "path_raster_stroke_spv.h"
    ;
alignas(4) const std::uint32_t kBinSpv[] =
#include "path_raster_bin_spv.h"
    ;
alignas(4) const std::uint32_t kFineSpv[] =
#include "path_raster_fine_spv.h"
    ;

} // namespace

namespace tessera::rendering::vulkan {

rhi::ComputePipelineDesc path_raster_pipeline_desc(PathRasterPass pass) {
    rhi::ComputePipelineDesc desc;
    switch (pass) {
    case PathRasterPass::Clear:
        desc.spirv = kClearSpv;
        desc.spirv_size = sizeof(kClearSpv);
        break;
    case PathRasterPass::Stroke:
        desc.spirv = kStrokeSpv;
        desc.spirv_size = sizeof(kStrokeSpv);
        break;
    case PathRasterPass::Bin:
        desc.spirv = kBinSpv;
        desc.spirv_size = sizeof(kBinSpv);
        break;
    case PathRasterPass::Fine:
        desc.spirv = kFineSpv;
        desc.spirv_size = sizeof(kFineSpv);
        break;
    default:
        throw std::invalid_argument("path_raster_pipeline_desc: unknown pass");
    }
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

} // namespace tessera::rendering::vulkan
