// SPDX-License-Identifier: MIT
//
// The recording sink of the per-node paint cache: it forwards every call to the real sink while
// capturing the node's emission as ops (and the pop state as flags, or as ops for the subtree
// draw list). Split out of paint_cache.cpp, which holds the keys, the LRU and the replay.

#include "tessera/ui/paint/paint_cache.hpp"

#include <utility>

namespace tessera::ui::paint {

PaintCache::Recorder::Recorder(PaintSink& target, bool record_pops)
    : target_(target),
      record_pops_(record_pops) {}

void PaintCache::Recorder::record(PaintOp op) {
    ops_.push_back(std::move(op));
}

void PaintCache::Recorder::fill_rect(const rhi::Rect& rect, const rhi::Color& color) {
    record(PaintOp{PaintOp::Kind::FillRect, FillRectOp{rect, color}});
    target_.fill_rect(rect, color);
}

void PaintCache::Recorder::fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color,
                                             float radius) {
    record(PaintOp{PaintOp::Kind::FillRoundedRect, FillRoundedRectOp{rect, color, radius}});
    target_.fill_rounded_rect(rect, color, radius);
}

void PaintCache::Recorder::fill_rounded_ring(const rhi::Rect& rect, const rhi::Color& color,
                                             float radius, float width) {
    record(PaintOp{PaintOp::Kind::FillRoundedRing, FillRoundedRingOp{rect, color, radius, width}});
    target_.fill_rounded_ring(rect, color, radius, width);
}

bool PaintCache::Recorder::supports_box_shadow() const {
    return target_.supports_box_shadow();
}

void PaintCache::Recorder::fill_box_shadow(const BoxShadowDraw& draw) {
    // The carve box rides in `carve`, whose radii already carry the border radius, so the record
    // needs no second rect.
    record(PaintOp{
        PaintOp::Kind::FillBoxShadow,
        FillBoxShadowOp{draw.box, draw.carve, draw.color, draw.radius, draw.sigma, draw.inset}});
    target_.fill_box_shadow(draw);
}

void PaintCache::Recorder::fill_linear_gradient(const GradientDraw& draw) {
    gradients_.emplace_back(draw);
    record(PaintOp{PaintOp::Kind::FillLinearGradient,
                   GradientRefOp{static_cast<std::uint32_t>(gradients_.size() - 1)}});
    if (!in_mask_ || mask_live_) {
        target_.fill_linear_gradient(draw);
    }
}

void PaintCache::Recorder::fill_conic_gradient(const ConicGradientDraw& draw) {
    gradients_.emplace_back(draw);
    record(PaintOp{PaintOp::Kind::FillConicGradient,
                   GradientRefOp{static_cast<std::uint32_t>(gradients_.size() - 1)}});
    if (!in_mask_ || mask_live_) {
        target_.fill_conic_gradient(draw);
    }
}

void PaintCache::Recorder::fill_radial_gradient(const RadialGradientDraw& draw) {
    gradients_.emplace_back(draw);
    record(PaintOp{PaintOp::Kind::FillRadialGradient,
                   GradientRefOp{static_cast<std::uint32_t>(gradients_.size() - 1)}});
    if (!in_mask_ || mask_live_) {
        target_.fill_radial_gradient(draw);
    }
}

void PaintCache::Recorder::push_clip(const RoundedClip& clip) {
    record(PaintOp{PaintOp::Kind::PushClip, PushClipOp{clip}});
    clip_ = clip;
    pushed_clip_ = true;
    target_.push_clip(clip);
}

void PaintCache::Recorder::pop_clip() {
    if (record_pops_) {
        record(PaintOp{PaintOp::Kind::PopClip, FillRectOp{}});
    }
    target_.pop_clip();
}

bool PaintCache::Recorder::supports_layers() const {
    return target_.supports_layers();
}

// Like a clip: the push is an op, the pop a flag on a per-node record (the pass closes the layer
// after the children) and an explicit op in the subtree draw list.
void PaintCache::Recorder::push_layer(const LayerDraw& layer) {
    record(PaintOp{PaintOp::Kind::PushLayer, PushLayerOp{layer}});
    pushed_layer_ = true;
    target_.push_layer(layer);
}

void PaintCache::Recorder::pop_layer() {
    if (record_pops_) {
        record(PaintOp{PaintOp::Kind::PopLayer, FillRectOp{}});
    }
    target_.pop_layer();
}

// A mask is recorded only into a subtree draw list (`record_pops`): a masked node is never recorded
// per node, because its mask paints after its children. The gradient fills between the two calls
// are always recorded so a later replay on a capable sink still masks, and are forwarded only while
// the target agreed to mask (see paint_cache.hpp `in_mask_`).
bool PaintCache::Recorder::begin_layer_mask() {
    if (!record_pops_) {
        return target_.begin_layer_mask();
    }
    record(PaintOp{PaintOp::Kind::BeginLayerMask, FillRectOp{}});
    in_mask_ = true;
    mask_live_ = target_.begin_layer_mask();
    return true;
}

void PaintCache::Recorder::end_layer_mask() {
    if (!record_pops_) {
        target_.end_layer_mask();
        return;
    }
    record(PaintOp{PaintOp::Kind::EndLayerMask, FillRectOp{}});
    if (mask_live_) {
        target_.end_layer_mask();
    }
    in_mask_ = false;
    mask_live_ = false;
}

bool PaintCache::Recorder::supports_transform() const {
    return target_.supports_transform();
}

bool PaintCache::Recorder::supports_backdrop() const {
    return target_.supports_backdrop();
}

void PaintCache::Recorder::push_transform(const rhi::Mat3& transform) {
    record(PaintOp{PaintOp::Kind::PushTransform, PushTransformOp{transform}});
    pushed_transform_ = true;
    target_.push_transform(transform);
}

void PaintCache::Recorder::pop_transform() {
    if (record_pops_) {
        record(PaintOp{PaintOp::Kind::PopTransform, FillRectOp{}});
    }
    target_.pop_transform();
}

void PaintCache::Recorder::draw_text(const TextDraw& run) {
    DrawTextOp draw;
    draw.rect = run.rect;
    if (!run.text.empty()) {
        draw.text.assign(run.text.data(), run.text.size());
    }
    draw.font_size = run.font_size;
    draw.font_weight = run.font_weight;
    draw.color = run.color;
    draw.baseline = run.baseline;
    draw.lcd = run.lcd;
    record(PaintOp{PaintOp::Kind::DrawText, std::move(draw)});
    target_.draw_text(run);
}

void PaintCache::Recorder::draw_vector(const VectorDraw& draw) {
    DrawAssetOp asset;
    asset.rect = draw.rect;
    if (!draw.asset.empty()) {
        asset.asset.assign(draw.asset.data(), draw.asset.size());
    }
    asset.raster_width = draw.raster_width;
    asset.raster_height = draw.raster_height;
    asset.scale = draw.scale;
    asset.tint = draw.tint;
    record(PaintOp{PaintOp::Kind::DrawVector, std::move(asset)});
    target_.draw_vector(draw);
}

void PaintCache::Recorder::draw_image(const ImageDraw& draw) {
    DrawAssetOp asset;
    asset.rect = draw.rect;
    if (!draw.asset.empty()) {
        asset.asset.assign(draw.asset.data(), draw.asset.size());
    }
    asset.raster_width = draw.raster_width;
    asset.raster_height = draw.raster_height;
    asset.scale = draw.scale;
    asset.tint = draw.tint;
    asset.decode_original = draw.decode_original;
    record(PaintOp{PaintOp::Kind::DrawImage, std::move(asset)});
    target_.draw_image(draw);
}

void PaintCache::Recorder::draw_backdrop(const BackdropDraw& draw) {
    record(PaintOp{PaintOp::Kind::DrawBackdrop, DrawBackdropOp{draw}});
    target_.draw_backdrop(draw);
}

PaintRecord PaintCache::Recorder::finish() {
    PaintRecord record;
    record.ops = std::move(ops_);
    record.gradients = std::move(gradients_);
    record.pushed_transform = pushed_transform_;
    record.pushed_clip = pushed_clip_;
    record.pushed_layer = pushed_layer_;
    record.clip = clip_;
    return record;
}

} // namespace tessera::ui::paint
