// SPDX-License-Identifier: MIT
//
// Bounded per-node paint cache: deterministic key validation, deterministic LRU eviction, exact
// byte accounting, the recording sink that captures one node's emission, and the replay used on a
// hit. Key rationale and the bypass list live in paint_cache.hpp.

#include "tessera/ui/paint/paint_cache.hpp"

#include <cstring>
#include <iterator>
#include <utility>

namespace tessera::ui::paint {
namespace {

bool same_rect(const rhi::Rect& lhs, const rhi::Rect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

bool same_matrix(const rhi::Mat3& lhs, const rhi::Mat3& rhs) {
    for (int index = 0; index < 9; ++index) {
        if (lhs.m[index] != rhs.m[index]) {
            return false;
        }
    }
    return true;
}

bool same_signature_impl(const PaintCacheSignature& lhs, const PaintCacheSignature& rhs) {
    return lhs.node == rhs.node && same_rect(lhs.bounds, rhs.bounds) &&
           same_matrix(lhs.transform, rhs.transform) && lhs.scrollable_x == rhs.scrollable_x &&
           lhs.scrollable_y == rhs.scrollable_y && lhs.scroll_x == rhs.scroll_x &&
           lhs.scroll_y == rhs.scroll_y && lhs.scrollport_left == rhs.scrollport_left &&
           lhs.scrollport_top == rhs.scrollport_top &&
           lhs.scrollport_right == rhs.scrollport_right &&
           lhs.scrollport_bottom == rhs.scrollport_bottom && lhs.interpolated == rhs.interpolated &&
           same_matrix(lhs.painted_world, rhs.painted_world) &&
           lhs.frame_active == rhs.frame_active &&
           same_matrix(lhs.frame_inverse, rhs.frame_inverse) &&
           lhs.sink_supports_transform == rhs.sink_supports_transform &&
           same_rect(lhs.paint_rect, rhs.paint_rect) && lhs.clip_hash == rhs.clip_hash &&
           lhs.clip_depth == rhs.clip_depth && lhs.opacity_bits == rhs.opacity_bits &&
           lhs.metrics_source_id == rhs.metrics_source_id &&
           lhs.device_scale_bits == rhs.device_scale_bits &&
           lhs.reduce_effects == rhs.reduce_effects && lhs.high_contrast == rhs.high_contrast &&
           lhs.lcd_text == rhs.lcd_text && lhs.glass_max_blur_radius == rhs.glass_max_blur_radius &&
           lhs.glass_max_backdrop_area == rhs.glass_max_backdrop_area;
}

std::uint64_t mix(std::uint64_t seed, std::uint64_t value) {
    // FNV-1a: one multiply per 64-bit word; the byte-level oracle is irrelevant here because the
    // seed and value sequence is fixed, only cross-run determinism and collision resistance matter.
    seed ^= value;
    seed *= 1099511628211ull;
    return seed;
}

std::uint64_t float_bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::size_t owned_string_bytes(const std::optional<PaintData>& payload) {
    if (!payload.has_value()) {
        return 0;
    }
    std::size_t bytes = payload->text.capacity();
    if (payload->decoration.has_value()) {
        bytes += payload->decoration->preedit_text.capacity();
    }
    return bytes;
}

std::size_t op_bytes(const PaintOp& op) {
    if (const auto* text = std::get_if<DrawTextOp>(&op.payload)) {
        return text->text.capacity();
    }
    if (const auto* asset = std::get_if<DrawAssetOp>(&op.payload)) {
        return asset->asset.capacity();
    }
    return 0;
}

} // namespace

// Exact equality, including floats: the record is replayed verbatim, so an epsilon match would be a
// silent output change. NaN can never match itself, which keeps a non-finite node a stable miss.
bool same_paint_signature(const PaintCacheSignature& lhs, const PaintCacheSignature& rhs) {
    return same_signature_impl(lhs, rhs);
}

std::uint64_t extend_paint_clip_hash(std::uint64_t seed, const RoundedClip& clip) {
    seed = mix(seed, float_bits(clip.rect.x));
    seed = mix(seed, float_bits(clip.rect.y));
    seed = mix(seed, float_bits(clip.rect.width));
    seed = mix(seed, float_bits(clip.rect.height));
    seed = mix(seed, float_bits(clip.top_left));
    seed = mix(seed, float_bits(clip.top_right));
    seed = mix(seed, float_bits(clip.bottom_right));
    seed = mix(seed, float_bits(clip.bottom_left));
    return seed;
}

void replay_paint_ops(const std::vector<PaintOp>& ops,
                      const std::vector<GradientPayload>& gradients, PaintSink& sink) {
    for (const PaintOp& op : ops) {
        switch (op.kind) {
        case PaintOp::Kind::FillRect: {
            const auto& draw = std::get<FillRectOp>(op.payload);
            sink.fill_rect(draw.rect, draw.color);
            break;
        }
        case PaintOp::Kind::FillRoundedRect: {
            const auto& draw = std::get<FillRoundedRectOp>(op.payload);
            sink.fill_rounded_rect(draw.rect, draw.color, draw.radius);
            break;
        }
        case PaintOp::Kind::FillRoundedRing: {
            const auto& draw = std::get<FillRoundedRingOp>(op.payload);
            sink.fill_rounded_ring(draw.rect, draw.color, draw.radius, draw.width);
            break;
        }
        case PaintOp::Kind::FillBoxShadow: {
            const auto& draw = std::get<FillBoxShadowOp>(op.payload);
            BoxShadowDraw shadow;
            shadow.box = draw.box;
            shadow.carve = draw.carve;
            shadow.color = draw.color;
            shadow.radius = draw.radius;
            shadow.sigma = draw.sigma;
            shadow.inset = draw.inset;
            sink.fill_box_shadow(shadow);
            break;
        }
        case PaintOp::Kind::FillLinearGradient: {
            const auto& ref = std::get<GradientRefOp>(op.payload);
            sink.fill_linear_gradient(std::get<GradientDraw>(gradients[ref.index]));
            break;
        }
        case PaintOp::Kind::FillRadialGradient: {
            const auto& ref = std::get<GradientRefOp>(op.payload);
            sink.fill_radial_gradient(std::get<RadialGradientDraw>(gradients[ref.index]));
            break;
        }
        case PaintOp::Kind::FillConicGradient: {
            const auto& ref = std::get<GradientRefOp>(op.payload);
            sink.fill_conic_gradient(std::get<ConicGradientDraw>(gradients[ref.index]));
            break;
        }
        case PaintOp::Kind::PushClip:
            sink.push_clip(std::get<PushClipOp>(op.payload).clip);
            break;
        case PaintOp::Kind::PushTransform:
            sink.push_transform(std::get<PushTransformOp>(op.payload).transform);
            break;
        case PaintOp::Kind::DrawText: {
            const auto& draw = std::get<DrawTextOp>(op.payload);
            TextDraw run;
            run.rect = draw.rect;
            run.text = draw.text;
            run.font_size = draw.font_size;
            run.font_weight = draw.font_weight;
            run.color = draw.color;
            run.baseline = draw.baseline;
            run.lcd = draw.lcd;
            sink.draw_text(run);
            break;
        }
        case PaintOp::Kind::DrawVector: {
            const auto& draw = std::get<DrawAssetOp>(op.payload);
            VectorDraw vector;
            vector.rect = draw.rect;
            vector.asset = draw.asset;
            vector.raster_width = draw.raster_width;
            vector.raster_height = draw.raster_height;
            vector.scale = draw.scale;
            vector.tint = draw.tint;
            sink.draw_vector(vector);
            break;
        }
        case PaintOp::Kind::DrawImage: {
            const auto& draw = std::get<DrawAssetOp>(op.payload);
            ImageDraw image;
            image.rect = draw.rect;
            image.asset = draw.asset;
            image.raster_width = draw.raster_width;
            image.raster_height = draw.raster_height;
            image.scale = draw.scale;
            image.tint = draw.tint;
            image.decode_original = draw.decode_original;
            sink.draw_image(image);
            break;
        }
        case PaintOp::Kind::DrawBackdrop:
            sink.draw_backdrop(std::get<DrawBackdropOp>(op.payload).draw);
            break;
        case PaintOp::Kind::PopClip:
            sink.pop_clip();
            break;
        case PaintOp::Kind::PopTransform:
            sink.pop_transform();
            break;
        }
    }
}

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
    target_.fill_linear_gradient(draw);
}

void PaintCache::Recorder::fill_conic_gradient(const ConicGradientDraw& draw) {
    gradients_.emplace_back(draw);
    record(PaintOp{PaintOp::Kind::FillConicGradient,
                   GradientRefOp{static_cast<std::uint32_t>(gradients_.size() - 1)}});
    target_.fill_conic_gradient(draw);
}

void PaintCache::Recorder::fill_radial_gradient(const RadialGradientDraw& draw) {
    gradients_.emplace_back(draw);
    record(PaintOp{PaintOp::Kind::FillRadialGradient,
                   GradientRefOp{static_cast<std::uint32_t>(gradients_.size() - 1)}});
    target_.fill_radial_gradient(draw);
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
    record.clip = clip_;
    return record;
}

PaintCache::PaintCache(PaintCacheConfig config)
    : config_(config) {}

bool PaintCache::enabled() const {
    return config_.max_entries > 0 && config_.max_bytes > 0 && enabled_;
}

void PaintCache::set_enabled(bool enabled) {
    enabled_ = enabled;
}

void PaintCache::note_bypass() {
    ++counters_.bypasses;
}

void PaintCache::note_miss() {
    ++counters_.misses;
}

const PaintRecord* PaintCache::find(const PaintCacheSignature& signature,
                                    const std::optional<PaintData>& payload) {
    if (!enabled()) {
        ++counters_.misses;
        return nullptr;
    }
    const auto found = entries_.find(signature.node);
    if (found == entries_.end() || !same_paint_signature(found->second.signature, signature) ||
        !(found->second.payload == payload)) {
        ++counters_.misses;
        return nullptr;
    }
    // Most-recently-used sits at the back; a hit only splices the list node (no allocation).
    lru_.splice(lru_.end(), lru_, found->second.lru);
    ++counters_.hits;
    const PaintRecord& record = found->second.record;
    counters_.replayed_draws +=
        record.ops.size() + (record.pushed_clip ? 1 : 0) + (record.pushed_transform ? 1 : 0);
    return &record;
}

void PaintCache::insert(PaintCacheSignature signature, const std::optional<PaintData>& payload,
                        PaintRecord record) {
    if (!enabled()) {
        return;
    }
    const NodeId node = signature.node;
    if (const auto existing = entries_.find(node); existing != entries_.end()) {
        bytes_ -= existing->second.bytes;
        lru_.erase(existing->second.lru);
        entries_.erase(existing);
    }

    const std::size_t bytes = entry_bytes(signature, payload, record);
    const std::size_t calls =
        record.ops.size() + (record.pushed_clip ? 1 : 0) + (record.pushed_transform ? 1 : 0);
    // Evict until the new entry fits both budgets. An entry larger than the whole byte budget is
    // not stored (the newest entry is only guaranteed to be kept when it fits).
    while (!entries_.empty() &&
           (entries_.size() + 1 > config_.max_entries || bytes_ + bytes > config_.max_bytes)) {
        evict_one();
    }
    if (entries_.size() + 1 > config_.max_entries || bytes_ + bytes > config_.max_bytes) {
        return;
    }

    Entry entry;
    entry.signature = std::move(signature);
    entry.payload = payload;
    entry.record = std::move(record);
    entry.bytes = bytes;

    lru_.push_back(node);
    entry.lru = std::prev(lru_.end());
    bytes_ += bytes;
    entries_.emplace(node, std::move(entry));
    counters_.recorded_draws += calls;
}

void PaintCache::erase(NodeId node) {
    const auto found = entries_.find(node);
    if (found == entries_.end()) {
        return;
    }
    bytes_ -= found->second.bytes;
    lru_.erase(found->second.lru);
    entries_.erase(found);
}

void PaintCache::invalidate() {
    ++counters_.invalidations;
    clear();
}

void PaintCache::clear() {
    entries_.clear();
    lru_.clear();
    bytes_ = 0;
}

void PaintCache::evict_one() {
    if (lru_.empty()) {
        return;
    }
    const NodeId victim = lru_.front();
    lru_.pop_front();
    if (const auto found = entries_.find(victim); found != entries_.end()) {
        bytes_ -= found->second.bytes;
        entries_.erase(found);
    }
    ++counters_.evictions;
}

std::size_t PaintCache::entry_bytes(const PaintCacheSignature& /*signature*/,
                                    const std::optional<PaintData>& payload,
                                    const PaintRecord& record) {
    std::size_t bytes = sizeof(Entry) + sizeof(NodeId) + 2 * sizeof(void*);
    bytes += record.ops.capacity() * sizeof(PaintOp);
    // The gradient side table (issue #731): charged at its own element size, which is what keeps
    // the budget honest now that a gradient no longer inflates every op.
    bytes += record.gradients.capacity() * sizeof(GradientPayload);
    bytes += owned_string_bytes(payload);
    for (const PaintOp& op : record.ops) {
        bytes += op_bytes(op);
    }
    return bytes;
}

PaintCacheStats PaintCache::stats() const {
    PaintCacheStats result = counters_;
    result.entries = entries_.size();
    result.bytes = bytes_;
    return result;
}

} // namespace tessera::ui::paint
