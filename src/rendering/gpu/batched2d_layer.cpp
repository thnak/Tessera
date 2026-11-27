// SPDX-License-Identifier: MIT
//
// Isolated layers of Batched2DRenderer (issue #227, docs/design/gpu/offscreen-layers.md section
// 5.2). A layer is the glass seam with a different destination: break the frame pass, open a pass
// on a pooled target the size of the frame (cleared to transparent), let the subtree draw into it
// under the frame's own transform, clips and scissors, then break again, reopen the frame pass and
// composite the target once with the group's opacity (glass_blur.frag mode 2).
//
// A layer that cannot be isolated paints flat: the opacity multiplies into every primitive until
// the matching pop (the non-isolated rule of paint-pass.md 14.1). Nothing throws or blocks on the
// frame path, and a tree that never pushes a layer records exactly the commands it did before.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace tessera::rendering::gpu {

bool Batched2DRenderer::layers_supported() const {
    return glass_.layers_configured();
}

rhi::Scissor Batched2DRenderer::device_region(const rhi::Rect& box) const {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    const float xs[4] = {box.x, box.right(), box.right(), box.x};
    const float ys[4] = {box.y, box.y, box.bottom(), box.bottom()};
    for (int corner = 0; corner < 4; ++corner) {
        float px = 0.0f;
        float py = 0.0f;
        transform_.transform_point(xs[corner], ys[corner], px, py);
        px *= content_scale_x_;
        py *= content_scale_y_;
        if (corner == 0) {
            min_x = max_x = px;
            min_y = max_y = py;
        } else {
            min_x = std::min(min_x, px);
            max_x = std::max(max_x, px);
            min_y = std::min(min_y, py);
            max_y = std::max(max_y, py);
        }
    }
    const float frame_width = static_cast<float>(extent_.width);
    const float frame_height = static_cast<float>(extent_.height);
    const float left = std::clamp(std::floor(min_x), 0.0f, frame_width);
    const float top = std::clamp(std::floor(min_y), 0.0f, frame_height);
    const float right = std::clamp(std::ceil(max_x), left, frame_width);
    const float bottom = std::clamp(std::ceil(max_y), top, frame_height);
    return rhi::Scissor{static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                        static_cast<std::uint32_t>(right - left),
                        static_cast<std::uint32_t>(bottom - top)};
}

namespace {

// How far (device px) a filter chain's output can land from the layer's own pixels: each blur
// reads 3 sigma around a pixel and each drop-shadow also moves it by its offset.
float filter_reach(const LayerFilterChain& filter, float scale) {
    float reach = 0.0f;
    for (std::size_t index = 0; index < filter.count; ++index) {
        const LayerFilterOp& op = filter.ops[index];
        if (op.kind == LayerFilterOp::Kind::ColorFunctions) {
            continue; // its fields hold amounts, and a colour function moves no pixel
        }
        reach += (3.0f * op.sigma + std::hypot(op.offset_x, op.offset_y)) * scale;
    }
    return reach;
}

rhi::Scissor grow(const rhi::Scissor& region, float by) {
    const auto pad = static_cast<std::int32_t>(std::ceil(by));
    return rhi::Scissor{region.x - pad, region.y - pad,
                        region.width + 2 * static_cast<std::uint32_t>(pad),
                        region.height + 2 * static_cast<std::uint32_t>(pad)};
}

} // namespace

void Batched2DRenderer::push_layer(const rhi::Rect& box, float opacity,
                                   const LayerFilterChain& filter) {
    // A retain_layer() call arms exactly one push; whatever it armed is spent here.
    const LayerRetainPending pending = std::exchange(retain_.pending, LayerRetainPending{});
    if (pending.hit) {
        push_retained_layer(pending, opacity);
        return;
    }
    LayerFrame layer;
    layer.opacity = std::isfinite(opacity) ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
    layer.previous_alpha_scale = alpha_scale_;

    rhi::RenderTarget* target = nullptr;
    rhi::Scissor region{};
    if (in_frame_ && commands_ != nullptr && frame_target_ != nullptr && !pass_broken_ &&
        glass_.layers_supported(*frame_target_)) {
        // The layer only ever needs the part of its box the frame can show: inside the frame
        // scissor and the damage bounds. An empty intersection has nothing to composite.
        const float scale = (content_scale_x_ + content_scale_y_) * 0.5f;
        region = rhi::intersect(grow(device_region(box), filter_reach(filter, scale)), scissor_);
        // A layer that will be kept renders its whole box so the pixels serve any later damage;
        // every other layer only what this frame's damage can show.
        if (pending.record && region.width > 0 && region.height > 0) {
            layer.retain.target = begin_retain_record(pending, region);
        }
        if (layer.retain.target == nullptr) {
            region = rhi::intersect(region, damage_bounds_);
        }
        if (region.width > 0 && region.height > 0) {
            target = glass_.acquire(extent_);
        }
        if (target == nullptr && layer.retain.target != nullptr) {
            retain_.cache->abandon(pending.owner);
            layer.retain.target = nullptr;
        }
    }

    if (target == nullptr) {
        // Flat fallback: the group still fades, per primitive.
        alpha_scale_ *= layer.opacity;
        layer_filters_unsupported_ += filter.count;
        ++layers_flat_;
        layers_.push_back(layer);
        return;
    }

    // Everything recorded so far reaches the frame pass before it closes, and the layer pass opens
    // on the pooled target, cleared to transparent so the batched pipeline's straight-alpha blend
    // accumulates premultiplied colour with a correct alpha (mode 2 divides it back).
    flush();
    commands_->end_render_pass();
    layer.isolated = true;
    layer.parent_target = frame_target_;
    layer.parent_desc = frame_desc_;
    layer.layer_target = target;
    layer.region = region;
    layer.filter = filter;
    if (layer.retain.target != nullptr) {
        layer.retain.recording = true;
        layer.retain.owner = pending.owner;
        layer.retain.degraded_mark = degradations();
        if (!retain_.damage_suspended) {
            // The frame's damage must not cut the pixels that will be kept: draw the whole region
            // now, and let the composite (after finish_retain_record) apply the damage again.
            retain_.saved_regions.swap(damage_regions_);
            retain_.saved_bounds = damage_bounds_;
            damage_bounds_ = base_scissor_;
            retain_.damage_suspended = true;
            layer.retain.suspended_damage = true;
        }
    }

    rhi::RenderTargetDesc desc = frame_desc_;
    desc.extent = extent_;
    desc.sample_count = 1;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};
    layer.layer_desc = desc;
    frame_target_ = target;
    frame_desc_ = desc;
    commands_->begin_render_pass(*target, desc);
    bound_pipeline_ = nullptr;
    bind_frame_state();
    ++layers_isolated_;
    layers_.push_back(layer);
}

std::vector<rhi::Scissor> Batched2DRenderer::layer_scissors(const LayerFrame& layer) const {
    std::vector<rhi::Scissor> scissors;
    const rhi::Scissor limited = rhi::intersect(scissor_, layer.region);
    if (damage_regions_.empty()) {
        scissors.push_back(limited);
    } else {
        for (const rhi::Scissor& damage : damage_regions_) {
            const rhi::Scissor part = rhi::intersect(limited, damage);
            if (part.width > 0 && part.height > 0) {
                scissors.push_back(part);
            }
        }
    }
    return scissors;
}

void Batched2DRenderer::pop_layer() {
    if (layers_.empty()) {
        return;
    }
    if (layers_.back().isolated && in_frame_ && commands_ != nullptr) {
        run_layer_filters(layers_.back());
    }
    const LayerFrame layer = layers_.back();
    layers_.pop_back();
    if (layer.retain.retained) {
        pop_retained_layer(layer);
        return;
    }
    if (!layer.isolated) {
        alpha_scale_ = layer.previous_alpha_scale;
        return;
    }
    if (!in_frame_ || commands_ == nullptr) {
        return;
    }

    // Close the layer pass, return to the frame target (its pass stays broken until the composite
    // reopens it through the seam) and composite once per scissor.
    flush();
    if (!pass_broken_) {
        commands_->end_render_pass();
    }
    frame_target_ = layer.parent_target;
    frame_desc_ = layer.parent_desc;
    pass_broken_ = true;
    bound_pipeline_ = nullptr;
    if (layer.retain.recording) {
        finish_retain_record(layer);
    }

    const std::vector<rhi::Scissor> scissors = layer_scissors(layer);

    GlassCompositor::FrameContext frame;
    frame.commands = commands_;
    frame.target = frame_target_;
    frame.desc = frame_desc_;
    frame.extent = extent_;
    frame.scissor = scissor_;
    frame.content_scale_x = content_scale_x_;
    frame.content_scale_y = content_scale_y_;
    frame.seam = GlassCompositor::PassSeam{this, &Batched2DRenderer::resume_pass_callback};
    const bool composited =
        glass_.composite_layer(frame, *layer.layer_target, scissors, layer.opacity);
    // The composite is the layer's last use: the next layer of this frame may take its target.
    glass_.release(layer.layer_target);
    if (!composited) {
        // Unreachable while push_layer() checked layers_supported(); reopen the frame pass so the
        // recording stays well formed if a future change breaks the pairing.
        resume_pass();
        return;
    }
    bind_frame_state();
}

} // namespace tessera::rendering::gpu
