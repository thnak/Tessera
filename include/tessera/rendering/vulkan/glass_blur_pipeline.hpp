// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

namespace tessera::rendering::vulkan {

// Pipeline identity for the glass backdrop blur (docs/specs/glass-v1.md section 5): the same
// `glass_blur` program in both passes, so the only variable is the frame target's format.
//
// The shader is `fullscreen.vert` (a triangle from gl_VertexIndex, no vertex buffer) plus
// `glass_blur.frag`, whose push-constant block is `gpu::GlassPushConstants`. It reads one combined
// image sampler at descriptor set 0, binding 0 - a texture (`bind_texture`) or, since RHI 1.3, a
// sampled render target (`bind_sampled_target`).
[[nodiscard]] rhi::PipelineDesc
glass_blur_pipeline_desc(rhi::TextureFormat color_format = rhi::TextureFormat::R8G8B8A8Unorm);

} // namespace tessera::rendering::vulkan
