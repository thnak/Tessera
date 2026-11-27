// SPDX-License-Identifier: MIT
//
// Layer masks of Batched2DRenderer (issue #617, docs/design/gpu/offscreen-layers.md section 5.2).
// A mask is drawn like any other content, into a second pooled target cleared to transparent, so
// its seams, anti-aliasing and overlaps are the ordinary blend's. When it is complete the mask
// target is sampled into the layer target with `BlendMode::DestinationIn`: every channel of the
// (premultiplied) layer is scaled by the mask's alpha, which is `mask-mode: alpha` compositing.
// Nothing here throws or blocks; a mask that cannot run reports false and the group paints
// unmasked.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

namespace tessera::rendering::gpu {

bool Batched2DRenderer::begin_layer_mask() {
    if (layers_.empty() || !in_frame_ || commands_ == nullptr) {
        ++layer_masks_unsupported_;
        return false;
    }
    LayerFrame& layer = layers_.back();
    if (!layer.isolated || layer.masked || layer.mask_target != nullptr ||
        !glass_.masks_supported()) {
        ++layer_masks_unsupported_;
        return false;
    }
    // One layer holds one pooled target and its mask a second; a pool with none left falls back
    // before anything is recorded.
    rhi::RenderTarget* target = glass_.acquire(extent_);
    if (target == nullptr) {
        ++layer_masks_unsupported_;
        return false;
    }

    flush();
    commands_->end_render_pass();
    layer.mask_target = target;

    rhi::RenderTargetDesc desc = layer.layer_desc;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};
    frame_target_ = target;
    frame_desc_ = desc;
    commands_->begin_render_pass(*target, desc);
    bound_pipeline_ = nullptr;
    bind_frame_state();
    return true;
}

void Batched2DRenderer::end_layer_mask() {
    if (layers_.empty() || layers_.back().mask_target == nullptr || commands_ == nullptr) {
        return;
    }
    LayerFrame& layer = layers_.back();
    rhi::RenderTarget* mask = layer.mask_target;
    layer.mask_target = nullptr;

    // Close the mask pass and go back to the layer target; its pass stays broken until the mask
    // composite reopens it (LoadOp::Load) through the seam.
    flush();
    commands_->end_render_pass();
    frame_target_ = layer.layer_target;
    frame_desc_ = layer.layer_desc;
    pass_broken_ = true;
    bound_pipeline_ = nullptr;

    GlassCompositor::FrameContext frame;
    frame.commands = commands_;
    frame.target = frame_target_;
    frame.desc = frame_desc_;
    frame.extent = extent_;
    frame.scissor = scissor_;
    frame.content_scale_x = content_scale_x_;
    frame.content_scale_y = content_scale_y_;
    frame.seam = GlassCompositor::PassSeam{this, &Batched2DRenderer::resume_pass_callback};
    if (!glass_.mask_layer(frame, *mask, layer_scissors(layer))) {
        // Unreachable while begin_layer_mask() checked masks_supported(); reopen the layer pass so
        // the recording stays well formed.
        resume_pass();
        return;
    }
    layer.masked = true;
    ++layer_masks_applied_;
    bind_frame_state();
}

} // namespace tessera::rendering::gpu
