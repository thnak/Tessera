// SPDX-License-Identifier: MIT
//
// The glass backdrop composite of Batched2DRenderer (issue #244, docs/specs/glass-v1.md sections
// 3-5). One glass node is executed entirely inside the frame it belongs to:
//
//   flush pending geometry -> break the pass -> copy the node's 3-sigma dependency region out of
//   the frame target into a pooled target -> horizontal Gaussian into a second pooled target
//   sampling that copy -> resume the frame pass with LoadOp::Load -> vertical Gaussian + color
//   matrix + rounded-box composite back into the frame target, with the node opacity folded into
//   the composite alpha.
//
// Everything is recorded on the frame's own command list, so nothing waits and nothing blocks the
// UI thread. The two blur targets come from the bounded OffscreenTargetPool (#242): when it cannot
// hand out both, the draw reports false and the caller paints the node without the effect (the
// bounded-cost rule of glass-v1 section 8, never an unbounded fallback).
//
// The pass ordering is the one RHI 1.3 requires: `bind_sampled_target` is recorded outside a render
// pass, after `bind_pipeline` and before `begin_render_pass` (docs/design/gpu/offscreen-layers.md
// section 5.3). The two pipelines differ only in blend state: the horizontal pass writes
// straight-alpha blur data into a pooled target (no blending), the composite pass draws over the
// frame target with alpha blending so its alpha is the blend weight.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/offscreen_target_pool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tessera::rendering::gpu {
namespace {

// The glass shaders draw a fullscreen triangle from gl_VertexIndex: no vertex buffer, three
// vertices, one instance.
constexpr std::uint32_t kFullscreenVertices = 3;

// Mean of the two content-scale axes: the blur sigma is a scalar, so a render-target downgrade
// scales it by the average axis factor (the same approximation push_clip() uses for radii).
float mean_scale(float x, float y) {
    return (x + y) * 0.5f;
}

} // namespace

bool Batched2DRenderer::glass_supported() const {
    return glass_.blur != nullptr && glass_.composite != nullptr &&
           config_.max_offscreen_targets > 0;
}

bool Batched2DRenderer::draw_backdrop(const GlassBackdropDraw& draw) {
    if (!in_frame_ || commands_ == nullptr || frame_target_ == nullptr || !glass_supported()) {
        return false;
    }
    // A swapchain image is not a copy source (RHI 1.1) and not samplable (RHI 1.3), so a frame that
    // paints straight into one cannot run the effect. The window renders such a frame into an
    // offscreen target first; this check keeps the failure a documented fallback instead of a
    // throw.
    if (frame_target_->presentable()) {
        return false;
    }
    // The composite pipeline's sample count must match the pass it draws into; a renderer wired for
    // a multisampled frame without matching glass pipelines falls back instead of failing the
    // frame.
    if (glass_.composite->sample_count() != frame_target_->sample_count()) {
        return false;
    }

    // Paint coordinates -> target pixels: positions scale per axis, the sigma by the mean factor.
    const float scale_x = content_scale_x_;
    const float scale_y = content_scale_y_;
    const rhi::Rect box{draw.clip.rect.x * scale_x, draw.clip.rect.y * scale_y,
                        draw.clip.rect.width * scale_x, draw.clip.rect.height * scale_y};
    const float device_scale = draw.device_scale * mean_scale(scale_x, scale_y);
    const GlassBackdropRegion region =
        plan_glass_backdrop_region(box, draw.sigma, device_scale, config_.glass, extent_);
    if (!region.active) {
        return false;
    }

    // The region and the blur target are both requested from the pool only after the pass is
    // broken, and a failure resumes it before returning so the caller's recording continues.
    break_pass();
    rhi::RenderTarget* source =
        acquire_offscreen_target(rhi::Extent2D{region.source.width, region.source.height});
    rhi::RenderTarget* blur = acquire_offscreen_target(region.blur);
    if (source == nullptr || blur == nullptr) {
        resume_pass();
        return false;
    }

    const rhi::Extent2D source_extent = source->extent();
    const rhi::Extent2D blur_extent = blur->extent();
    const float blur_width = static_cast<float>(region.blur.width);
    const float blur_height = static_cast<float>(region.blur.height);
    const float region_width = static_cast<float>(region.source.width);
    const float region_height = static_cast<float>(region.source.height);

    // Copy the dependency region 1:1 into the pooled source target at its origin.
    commands_->copy_target_region(*frame_target_, region.source, *source, rhi::Offset2D{});

    // --- Horizontal pass: blur along x into the pooled blur target ---------------------------
    // texel_step is the source-space uv advance per destination pixel. The target may be larger
    // than the requested blur extent (the pool keeps its high-water size), so the written sub-rect
    // is the requested extent and the step maps it onto exactly the copied region:
    //   uv = fragcoord * texel_step, and one texel_step * offset is one blur texel in source space.
    GlassPushConstants horizontal;
    horizontal.texel_step_x = region_width / (static_cast<float>(source_extent.width) * blur_width);
    horizontal.texel_step_y =
        region_height / (static_cast<float>(source_extent.height) * blur_height);
    horizontal.direction_x = 1.0f;
    horizontal.direction_y = 0.0f;
    horizontal.sigma = region.kernel.sigma;
    horizontal.radius = static_cast<float>(region.kernel.radius);
    horizontal.mode = 0.0f;

    rhi::RenderTargetDesc blur_desc = frame_desc_;
    blur_desc.extent = blur_extent;
    blur_desc.sample_count = 1;
    blur_desc.load_op = rhi::LoadOp::Clear;
    blur_desc.clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};

    commands_->bind_pipeline(*glass_.blur);
    commands_->bind_sampled_target(0, *source);
    commands_->begin_render_pass(*blur, blur_desc);
    commands_->set_viewport(rhi::Viewport{0.0f, 0.0f, blur_width, blur_height});
    commands_->set_scissor(rhi::Scissor{0, 0, region.blur.width, region.blur.height});
    commands_->push_constants(&horizontal, sizeof(horizontal));
    commands_->draw(kFullscreenVertices);
    commands_->end_render_pass();

    // --- Vertical pass + composite back into the frame target ---------------------------------
    // The vertical pass renders at the region's placement in the frame target, so its fragcoords
    // are frame-absolute and region_origin/region_size map them onto the blurred data: the region
    // maps onto the written sub-rect of the (possibly larger) blur target.
    GlassPushConstants vertical;
    vertical.texel_step_x = 1.0f / static_cast<float>(blur_extent.width);
    vertical.texel_step_y = 1.0f / static_cast<float>(blur_extent.height);
    vertical.direction_x = 0.0f;
    vertical.direction_y = 1.0f;
    vertical.sigma = region.kernel.sigma;
    vertical.radius = static_cast<float>(region.kernel.radius);
    vertical.region_origin_x = static_cast<float>(region.source.x);
    vertical.region_origin_y = static_cast<float>(region.source.y);
    vertical.region_size_x = region_width * static_cast<float>(blur_extent.width) / blur_width;
    vertical.region_size_y = region_height * static_cast<float>(blur_extent.height) / blur_height;
    vertical.box_x = box.x;
    vertical.box_y = box.y;
    vertical.box_width = box.width;
    vertical.box_height = box.height;
    const float radius_scale = mean_scale(scale_x, scale_y);
    vertical.radius_tl = draw.clip.top_left * radius_scale;
    vertical.radius_tr = draw.clip.top_right * radius_scale;
    vertical.radius_br = draw.clip.bottom_right * radius_scale;
    vertical.radius_bl = draw.clip.bottom_left * radius_scale;
    vertical.saturate = std::isfinite(draw.saturate) ? std::max(draw.saturate, 0.0f) : 1.0f;
    vertical.brightness = std::isfinite(draw.brightness) ? std::max(draw.brightness, 0.0f) : 1.0f;
    vertical.contrast = std::isfinite(draw.contrast) ? std::max(draw.contrast, 0.0f) : 1.0f;
    vertical.mode = 1.0f;
    vertical.opacity = std::clamp(draw.opacity, 0.0f, 1.0f);

    commands_->bind_sampled_target(0, *blur);
    resume_pass();
    commands_->bind_pipeline(*glass_.composite);
    commands_->set_viewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(extent_.width),
                                          static_cast<float>(extent_.height)});
    // The composite only writes where the frame's own scissor (damage/scroll clips) allows it.
    const rhi::Scissor region_scissor{region.source.x, region.source.y, region.source.width,
                                      region.source.height};
    commands_->set_scissor(rhi::intersect(scissor_, region_scissor));
    commands_->push_constants(&vertical, sizeof(vertical));
    commands_->draw(kFullscreenVertices);
    // The composite bound its own pipeline and push constants inside the frame's pass: restore the
    // 2D state so geometry recorded after the glass node continues the pre-break stream.
    bind_frame_state();
    ++glass_draws_;
    return true;
}

} // namespace tessera::rendering::gpu
