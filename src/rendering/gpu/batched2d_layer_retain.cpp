// SPDX-License-Identifier: MIT
//
// Retained layers of Batched2DRenderer (issue #617, docs/design/gpu/offscreen-layers.md section
// 5.6). A static masked, filtered or faded group is rendered once into an exact-size target (a
// region copy of the finished layer: after its filter and mask, before its opacity); while the
// caller says it is unchanged, every later frame composites that target and draws nothing for the
// subtree, its mask or its filter.
//
// Three rules keep a retained frame identical to a fresh one:
//  - the target holds the layer's FULL box (clamped to the frame and the clip), never just the
//    damaged part, so it serves any later damage: while a layer records, the frame's damage is
//    switched off and restored before the composite;
//  - the key the caller supplies is mixed here with the frame's extent and content scale, the two
//    inputs of the pixels the paint pass cannot see;
//  - a layer whose content degraded while it rendered (a fallback ran, an image was still loading)
//    is not kept.
// Nothing here throws or blocks; every refusal is a counted fall back to drawing the layer.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace tessera::rendering::gpu {
namespace {

std::uint64_t mix(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    return seed;
}

} // namespace

std::uint32_t Batched2DRenderer::degradations() const {
    return retain_.degraded + layers_flat_ + layer_masks_unsupported_ + layer_filters_unsupported_;
}

RetainedLayerStats Batched2DRenderer::retained_layer_stats() const {
    return retain_.cache != nullptr ? retain_.cache->stats() : RetainedLayerStats{};
}

bool Batched2DRenderer::retain_layer(std::uint64_t owner, std::uint64_t key, bool clean) {
    retain_.pending = LayerRetainPending{};
    RetainedLayerCache* cache = retain_.cache.get();
    if (cache == nullptr || !cache->enabled() || !in_frame_ || commands_ == nullptr ||
        frame_target_ == nullptr || pass_broken_ || !glass_.layers_supported(*frame_target_)) {
        return false;
    }
    // The paint pass cannot see the frame's size or content scale, and both change the pixels.
    key = mix(key, extent_.width);
    key = mix(key, extent_.height);
    key = mix(key, std::bit_cast<std::uint32_t>(content_scale_x_));
    key = mix(key, std::bit_cast<std::uint32_t>(content_scale_y_));
    LayerRetainPending& pending = retain_.pending;
    pending.owner = owner;
    pending.key = key;
    if (!clean) {
        cache->invalidate(owner);
        return false;
    }
    pending.entry = cache->find(owner, key);
    if (pending.entry.target != nullptr) {
        pending.hit = true;
        return true;
    }
    pending.record = true;
    return false;
}

rhi::RenderTarget* Batched2DRenderer::begin_retain_record(const LayerRetainPending& pending,
                                                          const rhi::Scissor& region) {
    return retain_.cache->reserve(pending.owner, pending.key, region);
}

void Batched2DRenderer::push_retained_layer(const LayerRetainPending& pending, float opacity) {
    LayerFrame layer;
    layer.opacity = std::isfinite(opacity) ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
    layer.previous_alpha_scale = alpha_scale_;
    layer.retain.retained = true;
    layer.retain.owner = pending.owner;
    layer.retain.target = pending.entry.target;
    layer.retain.placement = pending.entry.region;
    // The pixels cover the region they were recorded for; only the part the frame shows now (the
    // clip and the damage) is composited.
    layer.region = rhi::intersect(rhi::intersect(pending.entry.region, scissor_), damage_bounds_);
    layers_.push_back(layer);
}

void Batched2DRenderer::pop_retained_layer(const LayerFrame& layer) {
    if (!in_frame_ || commands_ == nullptr || layer.region.width == 0 || layer.region.height == 0) {
        return;
    }
    // The composite is a draw in the frame pass: everything recorded so far reaches it first.
    flush();
    if (!pass_broken_) {
        commands_->end_render_pass();
    }
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
    const rhi::Offset2D origin{layer.retain.placement.x, layer.retain.placement.y};
    if (!glass_.composite_layer(frame, *layer.retain.target, layer_scissors(layer), layer.opacity,
                                origin)) {
        resume_pass(); // unreachable while retain_layer() checked layers_supported()
        return;
    }
    bind_frame_state();
}

void Batched2DRenderer::finish_retain_record(const LayerFrame& layer) {
    RetainedLayerCache& cache = *retain_.cache;
    if (layer.retain.suspended_damage) {
        damage_regions_.swap(retain_.saved_regions);
        retain_.saved_regions.clear();
        damage_bounds_ = retain_.saved_bounds;
        retain_.damage_suspended = false;
    }
    if (degradations() != layer.retain.degraded_mark || layer.layer_target == nullptr) {
        cache.abandon(layer.retain.owner);
        return;
    }
    // The layer pass is closed (the caller ended it) and the layer target holds the finished,
    // premultiplied pixels of the layer's whole region: copy them 1:1 into the reserved target.
    commands_->copy_target_region(*layer.layer_target, layer.region, *layer.retain.target,
                                  rhi::Offset2D{});
    cache.commit(layer.retain.owner);
}

} // namespace tessera::rendering::gpu
