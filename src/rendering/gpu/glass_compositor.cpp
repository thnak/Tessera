// SPDX-License-Identifier: MIT
//
// The glass backdrop composite of Batched2DRenderer (issue #244, docs/specs/glass-v1.md sections
// 3-5), extracted into gpu::GlassCompositor by the god-type audit (slice 8). One glass node is
// executed entirely inside the frame it belongs to:
//
//   copy the node's 3-sigma dependency region out of the frame target into a pooled target ->
//   horizontal Gaussian into a second pooled target sampling that copy -> vertical Gaussian + color
//   matrix + rounded-box composite back into the frame target, with the node opacity folded into
//   the composite alpha.
//
// The caller breaks the frame pass before compose() and resumes it after, because that state
// belongs to the frame rather than to the effect.
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

#include "tessera/rendering/gpu/glass_compositor.hpp"

#include <algorithm>
#include <cmath>

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

GlassCompositor::GlassCompositor(rhi::Device& device, GlassPipelines glass,
                                 OffscreenTargetPoolConfig pool, const GlassLimits& limits)
    : device_(&device),
      glass_(glass),
      limits_(limits),
      max_targets_(pool.max_targets) {
    // The pool is created here rather than on the first frame: unlike the renderer it used to live
    // in, a compositor exists only because the renderer always constructs one, so an unconditional
    // pool keeps the "no offscreen texture for a no-glass tree" property that matters (a target is
    // still only created when a break actually asks for one).
    if (max_targets_ > 0) {
        pool_ = std::make_unique<OffscreenTargetPool>(*device_, pool);
    }
}

bool GlassCompositor::supported() const {
    return glass_.blur != nullptr && glass_.composite != nullptr && pool_ != nullptr;
}

void GlassCompositor::begin_frame(std::uint64_t frame_index, std::uint32_t frames_in_flight,
                                  rhi::TextureFormat format) {
    frame_index_ = frame_index;
    draws_ = 0;
    if (pool_ != nullptr) {
        pool_->begin_frame(frame_index_, frames_in_flight, format);
    }
}

rhi::RenderTarget* GlassCompositor::acquire(rhi::Extent2D extent) {
    return pool_ != nullptr ? pool_->acquire(extent) : nullptr;
}

void GlassCompositor::release(const rhi::RenderTarget* target) {
    if (pool_ != nullptr) {
        pool_->release(target);
    }
}

void GlassCompositor::trim(bool all_complete) {
    if (pool_ != nullptr) {
        pool_->trim(all_complete);
    }
}

bool GlassCompositor::can_read_target(const rhi::RenderTarget& target) const {
    // A swapchain image is not a copy source (RHI 1.1) and not samplable (RHI 1.3), so a frame that
    // paints straight into one cannot run the effect. The window renders such a frame into an
    // offscreen target first; this check keeps the failure a documented fallback instead of a
    // throw.
    if (target.presentable()) {
        return false;
    }
    // The composite pipeline's sample count must match the pass it draws into; a renderer wired for
    // a multisampled frame without matching glass pipelines falls back instead of failing the
    // frame.
    return glass_.composite->sample_count() == target.sample_count();
}

bool GlassCompositor::layers_supported(const rhi::RenderTarget& frame_target) const {
    return glass_.composite != nullptr && pool_ != nullptr && frame_target.sample_count() == 1 &&
           glass_.composite->sample_count() == 1;
}

bool GlassCompositor::composite_layer(const FrameContext& frame, rhi::RenderTarget& layer,
                                      const std::vector<rhi::Scissor>& scissors, float opacity) {
    if (frame.commands == nullptr || frame.target == nullptr || !layers_supported(*frame.target)) {
        return false;
    }
    composite_with(*glass_.composite, frame, layer, scissors, opacity);
    return true;
}

bool GlassCompositor::mask_layer(const FrameContext& frame, rhi::RenderTarget& mask,
                                 const std::vector<rhi::Scissor>& scissors) {
    if (frame.commands == nullptr || frame.target == nullptr || !masks_supported() ||
        frame.target->sample_count() != 1) {
        return false;
    }
    // Opacity 1: the mask's own alpha is the whole factor, and the colour is ignored by the blend.
    composite_with(*glass_.mask, frame, mask, scissors, 1.0f);
    return true;
}

bool GlassCompositor::combine_mask_part(const FrameContext& frame, rhi::RenderTarget& part,
                                        MaskCompositeOp op,
                                        const std::vector<rhi::Scissor>& scissors) {
    if (frame.commands == nullptr || frame.target == nullptr || op == MaskCompositeOp::Add ||
        !mask_part_supported(op) || frame.target->sample_count() != 1) {
        return false;
    }
    // The part is the source and the mask so far is the destination; only alpha matters to a mask,
    // so the colour channels the blend leaves behind are never read.
    composite_with(*mask_part_pipeline(op), frame, part, scissors, 1.0f);
    return true;
}

void GlassCompositor::composite_with(rhi::Pipeline& pipeline, const FrameContext& frame,
                                     rhi::RenderTarget& source,
                                     const std::vector<rhi::Scissor>& scissors, float opacity) {
    rhi::CommandList& commands = *frame.commands;
    const rhi::Extent2D layer_extent = source.extent();
    // Mode 2 samples the source at uv = (fragcoord - region_origin) / region_size. The source
    // shares the frame's coordinate system (it is drawn with the same transform, clips and
    // scissors), so the origin is zero and the size is the target's own, which maps a fragment onto
    // its own texel even when the pooled target is larger than the frame.
    GlassPushConstants layer_constants;
    layer_constants.region_size_x = static_cast<float>(layer_extent.width);
    layer_constants.region_size_y = static_cast<float>(layer_extent.height);
    layer_constants.mode = 2.0f;
    layer_constants.opacity = std::clamp(opacity, 0.0f, 1.0f);

    // Same ordering as compose(): the pipeline is bound before the sampled bind so the descriptor
    // set is recorded against the composite layout, then the frame pass is reopened and the
    // pipeline rebound.
    commands.bind_pipeline(pipeline);
    commands.bind_sampled_target(0, source);
    frame.seam.reopen();
    commands.bind_pipeline(pipeline);
    commands.set_viewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(frame.extent.width),
                                        static_cast<float>(frame.extent.height)});
    commands.push_constants(&layer_constants, sizeof(layer_constants));
    for (const rhi::Scissor& scissor : scissors) {
        commands.set_scissor(scissor);
        commands.draw(kFullscreenVertices);
        ++draws_;
    }
}

bool GlassCompositor::compose(const FrameContext& frame, const GlassBackdropDraw& draw) {
    if (frame.commands == nullptr || frame.target == nullptr || !supported()) {
        return false;
    }
    if (!can_read_target(*frame.target)) {
        return false;
    }

    // Paint coordinates -> target pixels: positions scale per axis, the sigma by the mean factor.
    // The frame's content scale already folds the device scale, so it is the whole CSS -> target
    // factor; multiplying by a second device-scale input would double the device scale on a hi-DPI
    // window and grow the 3-sigma dependency region with it.
    const float scale_x = frame.content_scale_x;
    const float scale_y = frame.content_scale_y;
    const rhi::Rect box{draw.clip.rect.x * scale_x, draw.clip.rect.y * scale_y,
                        draw.clip.rect.width * scale_x, draw.clip.rect.height * scale_y};
    const float device_scale = mean_scale(scale_x, scale_y);
    const GlassBackdropRegion region =
        plan_glass_backdrop_region(box, draw.sigma, device_scale, limits_, frame.extent);
    if (!region.active) {
        return false;
    }

    // Both targets are requested only after the caller broke the pass; a failure here returns false
    // and the caller resumes the pass, so its recording continues either way.
    rhi::RenderTarget* source = acquire(rhi::Extent2D{region.source.width, region.source.height});
    rhi::RenderTarget* blur = acquire(region.blur);
    if (source == nullptr || blur == nullptr) {
        release(source);
        release(blur);
        return false;
    }

    rhi::CommandList& commands = *frame.commands;
    const rhi::Extent2D source_extent = source->extent();
    const rhi::Extent2D blur_extent = blur->extent();
    const float blur_width = static_cast<float>(region.blur.width);
    const float blur_height = static_cast<float>(region.blur.height);
    const float region_width = static_cast<float>(region.source.width);
    const float region_height = static_cast<float>(region.source.height);

    // Copy the dependency region 1:1 into the pooled source target at its origin.
    commands.copy_target_region(*frame.target, region.source, *source, rhi::Offset2D{});

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

    rhi::RenderTargetDesc blur_desc = frame.desc;
    blur_desc.extent = blur_extent;
    blur_desc.sample_count = 1;
    blur_desc.load_op = rhi::LoadOp::Clear;
    blur_desc.clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};

    commands.bind_pipeline(*glass_.blur);
    commands.bind_sampled_target(0, *source);
    commands.begin_render_pass(*blur, blur_desc);
    commands.set_viewport(rhi::Viewport{0.0f, 0.0f, blur_width, blur_height});
    commands.set_scissor(rhi::Scissor{0, 0, region.blur.width, region.blur.height});
    commands.push_constants(&horizontal, sizeof(horizontal));
    commands.draw(kFullscreenVertices);
    commands.end_render_pass();

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

    // The sampler is bound outside a pass (RHI 1.3), and a descriptor-set bind is recorded against
    // the pipeline layout current at that point. The glass pipeline is therefore bound first, so
    // the set-0 bind below is recorded against the glass layout rather than the batched2d one the
    // frame left current (ADR-068 slice 3 made the two layouts differ).
    commands.bind_pipeline(*glass_.composite);
    commands.bind_sampled_target(0, *blur);
    // Reopen the frame pass before recording the composite into it. reopen() rebinds the frame's
    // own pipeline and clip set, so the glass pipeline is rebound after it; the set-0 bind above
    // stays valid because it was recorded against the glass layout.
    frame.seam.reopen();
    commands.bind_pipeline(*glass_.composite);
    commands.set_viewport(rhi::Viewport{0.0f, 0.0f, static_cast<float>(frame.extent.width),
                                        static_cast<float>(frame.extent.height)});
    // The composite only writes where the frame's own scissor (damage/scroll clips) allows it.
    const rhi::Scissor region_scissor{region.source.x, region.source.y, region.source.width,
                                      region.source.height};
    commands.set_scissor(rhi::intersect(frame.scissor, region_scissor));
    commands.push_constants(&vertical, sizeof(vertical));
    commands.draw(kFullscreenVertices);
    ++draws_;
    // Both targets were last read by the passes recorded above, so a later node of this frame may
    // reuse them (a page of glass cards needs two targets, not two per card).
    release(source);
    release(blur);
    return true;
}

} // namespace tessera::rendering::gpu
