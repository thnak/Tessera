// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

namespace tessera::rendering::vulkan {

// Pipeline identity for batched2d: (color format, sample count). Cache callers key on both; the
// sample count must match the render target used with the pipeline.
rhi::PipelineDesc
batched2d_pipeline_desc(rhi::TextureFormat color_format = rhi::TextureFormat::R8G8B8A8Unorm,
                        std::uint32_t sample_count = 1);

} // namespace tessera::rendering::vulkan
