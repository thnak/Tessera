// SPDX-License-Identifier: MIT
//
// Layer filter passes of GlassCompositor (issue #617, docs/design/gpu/offscreen-layers.md section
// 5.2). A `filter: blur()` or `drop-shadow()` is the glass blur program run over a layer target
// instead of a backdrop copy:
//
//   horizontal  source -> scratch   glass_blur.frag mode 0, no blending (blur pipeline)
//   vertical    scratch -> dest     mode 0 into a transparent clear (blur), or
//                                   mode 1 through the DestinationIn pipeline over a destination
//                                   cleared to the shadow colour (drop-shadow)
//
// The layer holds premultiplied colour with a correct alpha, so a plain linear-filtered Gaussian
// over all four channels is the premultiplied blur CSS specifies. The drop-shadow needs no tint
// shader: the destination is cleared to the premultiplied shadow colour and the vertical pass
// multiplies it by the blurred, offset alpha (mode 1 samples at `frag - region_origin`, which is
// the offset), with a box large enough that the rounded-box coverage is 1 everywhere.
//
// The colour functions (brightness() ... opacity()) are a third use of the same program: mode 3
// copies the layer 1:1 through the functions in one pass, so they need no pipeline of their own.

#include "tessera/rendering/gpu/glass_compositor.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::rendering::gpu {
namespace {

constexpr std::uint32_t kFullscreenVertices = 3;
// Mode 1 multiplies by the rounded-box coverage; a box this large keeps every fragment inside it.
constexpr float kUnboundedBox = 1.0e5f;

float texel(std::uint32_t extent) {
    return 1.0f / static_cast<float>(std::max(extent, 1u));
}

} // namespace

bool GlassCompositor::blur_layer(const FrameContext& frame, rhi::RenderTarget& source,
                                 rhi::RenderTarget& scratch, rhi::RenderTarget& dest,
                                 const FilterPass& pass,
                                 const std::vector<rhi::Scissor>& scissors) {
    if (frame.commands == nullptr || !filters_supported(pass.shadow)) {
        return false;
    }
    rhi::CommandList& commands = *frame.commands;
    const GlassKernel kernel = make_glass_kernel(pass.sigma);

    rhi::RenderTargetDesc desc = frame.desc;
    desc.sample_count = 1;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};

    GlassPushConstants horizontal;
    horizontal.texel_step_x = texel(source.extent().width);
    horizontal.texel_step_y = texel(source.extent().height);
    horizontal.direction_x = 1.0f;
    horizontal.direction_y = 0.0f;
    horizontal.sigma = kernel.sigma;
    horizontal.radius = static_cast<float>(kernel.radius);
    horizontal.mode = 0.0f;

    desc.extent = scratch.extent();
    commands.bind_pipeline(*glass_.blur);
    commands.bind_sampled_target(0, source);
    commands.begin_render_pass(scratch, desc);
    commands.set_viewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(desc.extent.width),
                                        static_cast<float>(desc.extent.height)});
    commands.set_scissor(pass.first_region);
    commands.push_constants(&horizontal, sizeof(horizontal));
    commands.draw(kFullscreenVertices);
    commands.end_render_pass();

    GlassPushConstants vertical = horizontal;
    vertical.texel_step_x = texel(scratch.extent().width);
    vertical.texel_step_y = texel(scratch.extent().height);
    vertical.direction_x = 0.0f;
    vertical.direction_y = 1.0f;
    rhi::Pipeline* pipeline = glass_.blur;
    if (pass.shadow) {
        // Mode 1 reads uv = (frag - region_origin) / region_size, so the origin is the offset.
        pipeline = glass_.mask;
        vertical.mode = 1.0f;
        vertical.region_origin_x = pass.offset_x;
        vertical.region_origin_y = pass.offset_y;
        vertical.region_size_x = static_cast<float>(scratch.extent().width);
        vertical.region_size_y = static_cast<float>(scratch.extent().height);
        vertical.box_x = -kUnboundedBox;
        vertical.box_y = -kUnboundedBox;
        vertical.box_width = 2.0f * kUnboundedBox;
        vertical.box_height = 2.0f * kUnboundedBox;
        vertical.opacity = 1.0f;
        const rhi::Color& tint = pass.tint;
        desc.clear_color = rhi::Color{tint.r * tint.a, tint.g * tint.a, tint.b * tint.a, tint.a};
    }

    desc.extent = dest.extent();
    commands.bind_pipeline(*pipeline);
    commands.bind_sampled_target(0, scratch);
    commands.begin_render_pass(dest, desc);
    commands.set_viewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(desc.extent.width),
                                        static_cast<float>(desc.extent.height)});
    commands.push_constants(&vertical, sizeof(vertical));
    for (const rhi::Scissor& scissor : scissors) {
        commands.set_scissor(scissor);
        commands.draw(kFullscreenVertices);
        ++draws_;
    }
    commands.end_render_pass();
    return true;
}

bool GlassCompositor::color_layer(const FrameContext& frame, rhi::RenderTarget& source,
                                  rhi::RenderTarget& dest, const LayerFilterOp& op,
                                  const std::vector<rhi::Scissor>& scissors) {
    if (frame.commands == nullptr || !filters_supported(false) ||
        op.kind != LayerFilterOp::Kind::ColorFunctions) {
        return false;
    }
    rhi::CommandList& commands = *frame.commands;

    GlassPushConstants push;
    push.texel_step_x = texel(source.extent().width);
    push.texel_step_y = texel(source.extent().height);
    push.mode = 3.0f;
    push.radius_tl = static_cast<float>(op.color_function(0));
    push.radius_tr = static_cast<float>(op.color_function(1));
    push.radius_br = static_cast<float>(op.color_function(2));
    push.saturate = op.color_amount(0);
    push.brightness = op.color_amount(1);
    push.contrast = op.color_amount(2);

    rhi::RenderTargetDesc desc = frame.desc;
    desc.sample_count = 1;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};
    desc.extent = dest.extent();
    commands.bind_pipeline(*glass_.blur);
    commands.bind_sampled_target(0, source);
    commands.begin_render_pass(dest, desc);
    commands.set_viewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(desc.extent.width),
                                        static_cast<float>(desc.extent.height)});
    commands.push_constants(&push, sizeof(push));
    for (const rhi::Scissor& scissor : scissors) {
        commands.set_scissor(scissor);
        commands.draw(kFullscreenVertices);
        ++draws_;
    }
    commands.end_render_pass();
    return true;
}

} // namespace tessera::rendering::gpu
