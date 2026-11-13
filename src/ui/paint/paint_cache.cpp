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

// Exact equality, including floats: the record is replayed verbatim, so an epsilon match would be a
// silent output change. NaN can never match itself, which keeps a non-finite node a stable miss.
bool same_signature(const PaintCacheSignature& lhs, const PaintCacheSignature& rhs) {
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
           lhs.glass_max_blur_radius == rhs.glass_max_blur_radius &&
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
    return op.text.capacity();
}

} // namespace

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

void replay_paint_ops(const std::vector<PaintOp>& ops, PaintSink& sink) {
    for (const PaintOp& op : ops) {
        switch (op.kind) {
        case PaintOp::Kind::FillRect:
            sink.fill_rect(op.rect, op.color);
            break;
        case PaintOp::Kind::FillRoundedRect:
            sink.fill_rounded_rect(op.rect, op.color, op.radius);
            break;
        case PaintOp::Kind::FillLinearGradient:
            sink.fill_linear_gradient(op.gradient);
            break;
        case PaintOp::Kind::PushClip:
            sink.push_clip(op.clip);
            break;
        case PaintOp::Kind::PushTransform:
            sink.push_transform(op.transform);
            break;
        case PaintOp::Kind::DrawText: {
            TextDraw run;
            run.rect = op.rect;
            run.text = op.text;
            run.font_size = op.font_size;
            run.font_weight = op.font_weight;
            run.color = op.color;
            run.baseline = op.baseline;
            sink.draw_text(run);
            break;
        }
        case PaintOp::Kind::DrawVector: {
            VectorDraw draw;
            draw.rect = op.rect;
            draw.asset = op.text;
            draw.raster_width = op.raster_width;
            draw.raster_height = op.raster_height;
            draw.scale = op.scale;
            draw.tint = op.color;
            sink.draw_vector(draw);
            break;
        }
        case PaintOp::Kind::DrawImage: {
            ImageDraw draw;
            draw.rect = op.rect;
            draw.asset = op.text;
            draw.raster_width = op.raster_width;
            draw.raster_height = op.raster_height;
            draw.scale = op.scale;
            draw.tint = op.color;
            draw.decode_original = op.decode_original;
            sink.draw_image(draw);
            break;
        }
        case PaintOp::Kind::DrawBackdrop:
            sink.draw_backdrop(op.backdrop);
            break;
        }
    }
}

PaintCache::Recorder::Recorder(PaintSink& target)
    : target_(target) {}

void PaintCache::Recorder::record(PaintOp op) {
    ops_.push_back(std::move(op));
}

void PaintCache::Recorder::fill_rect(const rhi::Rect& rect, const rhi::Color& color) {
    PaintOp op;
    op.kind = PaintOp::Kind::FillRect;
    op.rect = rect;
    op.color = color;
    record(std::move(op));
    target_.fill_rect(rect, color);
}

void PaintCache::Recorder::fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color,
                                             float radius) {
    PaintOp op;
    op.kind = PaintOp::Kind::FillRoundedRect;
    op.rect = rect;
    op.color = color;
    op.radius = radius;
    record(std::move(op));
    target_.fill_rounded_rect(rect, color, radius);
}

void PaintCache::Recorder::fill_linear_gradient(const GradientDraw& draw) {
    PaintOp op;
    op.kind = PaintOp::Kind::FillLinearGradient;
    op.gradient = draw;
    record(std::move(op));
    target_.fill_linear_gradient(draw);
}

void PaintCache::Recorder::push_clip(const RoundedClip& clip) {
    PaintOp op;
    op.kind = PaintOp::Kind::PushClip;
    op.clip = clip;
    record(std::move(op));
    clip_ = clip;
    pushed_clip_ = true;
    target_.push_clip(clip);
}

void PaintCache::Recorder::pop_clip() {
    target_.pop_clip();
}

bool PaintCache::Recorder::supports_transform() const {
    return target_.supports_transform();
}

bool PaintCache::Recorder::supports_backdrop() const {
    return target_.supports_backdrop();
}

void PaintCache::Recorder::push_transform(const rhi::Mat3& transform) {
    PaintOp op;
    op.kind = PaintOp::Kind::PushTransform;
    op.transform = transform;
    record(std::move(op));
    pushed_transform_ = true;
    target_.push_transform(transform);
}

void PaintCache::Recorder::pop_transform() {
    target_.pop_transform();
}

void PaintCache::Recorder::draw_text(const TextDraw& run) {
    PaintOp op;
    op.kind = PaintOp::Kind::DrawText;
    op.rect = run.rect;
    if (!run.text.empty()) {
        op.text.assign(run.text.data(), run.text.size());
    }
    op.font_size = run.font_size;
    op.font_weight = run.font_weight;
    op.color = run.color;
    op.baseline = run.baseline;
    record(std::move(op));
    target_.draw_text(run);
}

void PaintCache::Recorder::draw_vector(const VectorDraw& draw) {
    PaintOp op;
    op.kind = PaintOp::Kind::DrawVector;
    op.rect = draw.rect;
    if (!draw.asset.empty()) {
        op.text.assign(draw.asset.data(), draw.asset.size());
    }
    op.raster_width = draw.raster_width;
    op.raster_height = draw.raster_height;
    op.scale = draw.scale;
    op.color = draw.tint;
    record(std::move(op));
    target_.draw_vector(draw);
}

void PaintCache::Recorder::draw_image(const ImageDraw& draw) {
    PaintOp op;
    op.kind = PaintOp::Kind::DrawImage;
    op.rect = draw.rect;
    if (!draw.asset.empty()) {
        op.text.assign(draw.asset.data(), draw.asset.size());
    }
    op.raster_width = draw.raster_width;
    op.raster_height = draw.raster_height;
    op.scale = draw.scale;
    op.color = draw.tint;
    op.decode_original = draw.decode_original;
    record(std::move(op));
    target_.draw_image(draw);
}

void PaintCache::Recorder::draw_backdrop(const BackdropDraw& draw) {
    PaintOp op;
    op.kind = PaintOp::Kind::DrawBackdrop;
    op.backdrop = draw;
    record(std::move(op));
    target_.draw_backdrop(draw);
}

PaintRecord PaintCache::Recorder::finish() {
    PaintRecord record;
    record.ops = std::move(ops_);
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
    if (found == entries_.end() || !same_signature(found->second.signature, signature) ||
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
