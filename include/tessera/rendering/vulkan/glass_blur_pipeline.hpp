// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

namespace tessera::rendering::vulkan {

// Pipeline identity for the glass backdrop blur (docs/specs/glass-v1.md section 5): the same
// `glass_blur` program in both passes, so the variables are the frame target's format and the blend
// state the pass needs.
//
// The shader is `fullscreen.vert` (a triangle from gl_VertexIndex, no vertex buffer) plus
// `glass_blur.frag`, whose push-constant block is `gpu::GlassPushConstants`. It reads one combined
// image sampler at descriptor set 0, binding 0 - a texture (`bind_texture`) or, since RHI 1.3, a
// sampled render target (`bind_sampled_target`).
//
// `BlendMode::None` is the horizontal pass (it writes straight-alpha blur data into a pooled
// target, so blending there would premultiply it); `BlendMode::Alpha` is the composite pass, where
// the shader's alpha is the blend weight that folds the node opacity in and antialiases the rounded
// clip.
[[nodiscard]] rhi::PipelineDesc
glass_blur_pipeline_desc(rhi::TextureFormat color_format = rhi::TextureFormat::R8G8B8A8Unorm,
                         rhi::BlendMode blend = rhi::BlendMode::Alpha);

} // namespace tessera::rendering::vulkan
