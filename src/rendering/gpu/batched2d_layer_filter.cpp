// SPDX-License-Identifier: MIT
//
// Layer filters of Batched2DRenderer (issue #617, docs/design/gpu/offscreen-layers.md section 5.2).
// A layer's `filter` chain runs once on its pooled target when the layer is finished - at
// begin_layer_mask() or pop_layer(), whichever comes first - so it sees the complete content and
// precedes the mask and the group opacity, as CSS orders them. A blur works in place through one
// scratch target; a drop-shadow builds the shadow into a third target, composites the layer over
// it and continues with that target as the layer. A function that cannot run (no blur or mask
// pipeline, no free pooled target) is skipped and counted; nothing throws or blocks.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace tessera::rendering::gpu {
namespace {

// The horizontal pass has to cover every row the vertical pass will read: the scissors' union,
// grown by the kernel reach and the shadow offset, inside the frame.
rhi::Scissor first_pass_region(const std::vector<rhi::Scissor>& scissors, float margin,
                               rhi::Extent2D frame) {
    std::int32_t left = INT32_MAX;
    std::int32_t top = INT32_MAX;
    std::int32_t right = INT32_MIN;
    std::int32_t bottom = INT32_MIN;
    for (const rhi::Scissor& scissor : scissors) {
        left = std::min(left, scissor.x);
        top = std::min(top, scissor.y);
        right = std::max(right, scissor.x + static_cast<std::int32_t>(scissor.width));
        bottom = std::max(bottom, scissor.y + static_cast<std::int32_t>(scissor.height));
    }
    const auto pad = static_cast<std::int32_t>(std::ceil(margin)) + 2;
    left = std::max(left - pad, 0);
    top = std::max(top - pad, 0);
    right = std::min(right + pad, static_cast<std::int32_t>(frame.width));
    bottom = std::min(bottom + pad, static_cast<std::int32_t>(frame.height));
    return rhi::Scissor{left, top, static_cast<std::uint32_t>(std::max(right - left, 0)),
                        static_cast<std::uint32_t>(std::max(bottom - top, 0))};
}

} // namespace

void Batched2DRenderer::run_layer_filters(LayerFrame& layer) {
    if (layer.filter.empty() || !layer.isolated || !in_frame_ || commands_ == nullptr) {
        return;
    }
    const LayerFilterChain chain = layer.filter;
    layer.filter = LayerFilterChain{};

    const std::vector<rhi::Scissor> scissors = layer_scissors(layer);
    bool runnable = false;
    for (std::size_t index = 0; index < chain.count; ++index) {
        runnable = runnable || glass_.filters_supported(chain.ops[index].kind ==
                                                        LayerFilterOp::Kind::DropShadow);
    }
    if (!runnable || scissors.empty()) {
        layer_filters_unsupported_ += runnable ? 0u : chain.count;
        return;
    }

    // The layer pass closes so each effect can sample the layer; the chain leaves it closed
    // (pass_broken_) with frame_target_ on whichever target is the layer now.
    flush();
    commands_->end_render_pass();
    pass_broken_ = true;
    bound_pipeline_ = nullptr;

    const float scale = (content_scale_x_ + content_scale_y_) * 0.5f;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    transform_.transform_point(0.0f, 0.0f, origin_x, origin_y);
    for (std::size_t index = 0; index < chain.count; ++index) {
        const LayerFilterOp& op = chain.ops[index];
        const bool shadow = op.kind == LayerFilterOp::Kind::DropShadow;
        if (!glass_.filters_supported(shadow)) {
            ++layer_filters_unsupported_;
            continue;
        }
        if (layer.scratch == nullptr) {
            layer.scratch = glass_.acquire(extent_);
        }
        rhi::RenderTarget* out = layer.layer_target;
        if (shadow) {
            if (layer.spare == nullptr) {
                layer.spare = glass_.acquire(extent_);
            }
            out = layer.spare;
        }
        if (layer.scratch == nullptr || out == nullptr) {
            ++layer_filters_unsupported_;
            continue;
        }

        GlassCompositor::FilterPass pass;
        pass.sigma = op.sigma * scale;
        pass.shadow = shadow;
        pass.tint = op.tint();
        if (shadow) {
            // The offset follows the active transform's linear part, in device pixels.
            float moved_x = 0.0f;
            float moved_y = 0.0f;
            transform_.transform_point(op.offset_x, op.offset_y, moved_x, moved_y);
            pass.offset_x = (moved_x - origin_x) * content_scale_x_;
            pass.offset_y = (moved_y - origin_y) * content_scale_y_;
        }
        pass.first_region = first_pass_region(
            scissors, 3.0f * pass.sigma + std::abs(pass.offset_x) + std::abs(pass.offset_y),
            extent_);

        GlassCompositor::FrameContext frame;
        frame.commands = commands_;
        frame.target = layer.layer_target;
        frame.desc = layer.layer_desc;
        frame.extent = extent_;
        frame.scissor = scissor_;
        frame.content_scale_x = content_scale_x_;
        frame.content_scale_y = content_scale_y_;
        frame.seam = GlassCompositor::PassSeam{this, &Batched2DRenderer::resume_pass_callback};
        if (!glass_.blur_layer(frame, *layer.layer_target, *layer.scratch, *out, pass, scissors)) {
            ++layer_filters_unsupported_;
            continue;
        }
        if (shadow) {
            // The layer goes over its own shadow: the straight-alpha composite onto a premultiplied
            // destination is ordinary source-over, and the shadow target becomes the layer.
            frame_target_ = out;
            frame.target = out;
            if (glass_.composite_layer(frame, *layer.layer_target, scissors, 1.0f)) {
                commands_->end_render_pass();
                pass_broken_ = true;
                bound_pipeline_ = nullptr;
                std::swap(layer.layer_target, layer.spare);
            }
            frame_target_ = layer.layer_target;
        }
        ++layer_filters_applied_;
    }

    // The chain's intermediates are dead: the scratch target was last read by the vertical pass,
    // and after a drop-shadow `spare` holds the layer's old target, last read by the composite over
    // its shadow. The mask and the next layer may reuse them.
    glass_.release(layer.scratch);
    glass_.release(layer.spare);
    layer.scratch = nullptr;
    layer.spare = nullptr;

    // Left broken on purpose: both callers close the layer pass next, so reopening it here would
    // record an empty pass.
    frame_target_ = layer.layer_target;
    frame_desc_ = layer.layer_desc;
}

} // namespace tessera::rendering::gpu
