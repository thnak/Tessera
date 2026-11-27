// SPDX-License-Identifier: MIT
//
// `mask-composite` layers of Batched2DRenderer (issue #617, docs/design/gpu/offscreen-layers.md
// section 5.2e). A mask layer that is not `add` cannot be blended quad by quad, because subtract,
// intersect and exclude drop the mask below it wherever the layer does not cover it. So the layer
// is drawn into its own pooled target cleared to transparent, and when it is complete that target
// is sampled into the mask target with the operator's blend (RHI 1.21 `SourceOut`, `DestinationIn`
// or `Xor`), once, over the layer's scissors. Nothing here throws or blocks; a layer that cannot
// run reports false and the caller drops it.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

namespace tessera::rendering::gpu {

bool Batched2DRenderer::begin_mask_layer(MaskCompositeOp op) {
    if (op == MaskCompositeOp::Add) {
        return true; // drawn straight into the mask; end_mask_layer() finds no part and returns
    }
    if (layers_.empty() || !in_frame_ || commands_ == nullptr ||
        layers_.back().mask_target == nullptr || layers_.back().part_target != nullptr ||
        pass_broken_ || !glass_.mask_part_supported(op)) {
        ++layer_masks_unsupported_;
        return false;
    }
    LayerFrame& layer = layers_.back();
    rhi::RenderTarget* part = glass_.acquire(extent_);
    if (part == nullptr) {
        ++layer_masks_unsupported_;
        return false;
    }
    flush();
    commands_->end_render_pass(); // the mask so far
    layer.part_target = part;
    layer.part_op = op;

    rhi::RenderTargetDesc desc = layer.layer_desc;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};
    frame_target_ = part;
    frame_desc_ = desc;
    commands_->begin_render_pass(*part, desc);
    pass_broken_ = false;
    bound_pipeline_ = nullptr;
    bind_frame_state();
    return true;
}

void Batched2DRenderer::end_mask_layer() {
    if (layers_.empty() || layers_.back().part_target == nullptr || commands_ == nullptr) {
        return;
    }
    LayerFrame& layer = layers_.back();
    rhi::RenderTarget* part = layer.part_target;
    layer.part_target = nullptr;

    // Close the part's pass and go back to the mask target; its pass stays broken until the
    // combine reopens it (LoadOp::Load) through the seam.
    flush();
    commands_->end_render_pass();
    frame_target_ = layer.mask_target;
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
    const bool combined =
        glass_.combine_mask_part(frame, *part, layer.part_op, layer_scissors(layer));
    glass_.release(part); // the combine is the part's last use
    if (!combined) {
        resume_pass(); // unreachable while begin_mask_layer() checked support; keep it well formed
        return;
    }
    bind_frame_state();
}

} // namespace tessera::rendering::gpu
