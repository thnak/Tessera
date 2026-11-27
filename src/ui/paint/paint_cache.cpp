// SPDX-License-Identifier: MIT
//
// Bounded per-node paint cache: deterministic key validation, deterministic LRU eviction, exact
// byte accounting and the replay used on a hit (the recording sink is paint_cache_recorder.cpp).
// Key rationale and the bypass list live in paint_cache.hpp.

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
           lhs.lcd_text == rhs.lcd_text && lhs.isolate_groups == rhs.isolate_groups &&
           lhs.in_layer == rhs.in_layer && lhs.glass_max_blur_radius == rhs.glass_max_blur_radius &&
           lhs.glass_max_backdrop_area == rhs.glass_max_backdrop_area &&
           lhs.list_ordinal == rhs.list_ordinal;
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
    // A recorded mask replays only while the sink agrees to mask: when it does not, everything up
    // to the matching EndLayerMask is skipped and the group paints unmasked, as it would live.
    bool skipping_mask = false;
    bool skipping_part = false; // a non-add mask layer the sink declined
    using Op = rendering::gpu::MaskCompositeOp;
    for (const PaintOp& op : ops) {
        if (skipping_mask && op.kind != PaintOp::Kind::EndLayerMask) {
            continue;
        }
        if (skipping_part && op.kind != PaintOp::Kind::EndMaskLayer) {
            continue;
        }
        switch (op.kind) {
        case PaintOp::Kind::BeginLayerMask:
            skipping_mask = !sink.begin_layer_mask();
            break;
        case PaintOp::Kind::BeginMaskSubtract:
            skipping_part = !sink.begin_mask_layer(Op::Subtract);
            break;
        case PaintOp::Kind::BeginMaskIntersect:
            skipping_part = !sink.begin_mask_layer(Op::Intersect);
            break;
        case PaintOp::Kind::BeginMaskExclude:
            skipping_part = !sink.begin_mask_layer(Op::Exclude);
            break;
        case PaintOp::Kind::EndMaskLayer:
            if (!skipping_part) {
                sink.end_mask_layer();
            }
            skipping_part = false;
            break;
        case PaintOp::Kind::EndLayerMask:
            if (!skipping_mask) {
                sink.end_layer_mask();
            }
            skipping_mask = false;
            break;
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
        case PaintOp::Kind::PushLayer:
            sink.push_layer(std::get<PushLayerOp>(op.payload).layer);
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
        case PaintOp::Kind::PopLayer:
            sink.pop_layer();
            break;
        }
    }
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
    counters_.replayed_draws += record.ops.size() + (record.pushed_clip ? 1 : 0) +
                                (record.pushed_transform ? 1 : 0) + (record.pushed_layer ? 1 : 0);
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
    const std::size_t calls = record.ops.size() + (record.pushed_clip ? 1 : 0) +
                              (record.pushed_transform ? 1 : 0) + (record.pushed_layer ? 1 : 0);
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
