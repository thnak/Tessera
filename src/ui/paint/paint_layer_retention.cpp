// SPDX-License-Identifier: MIT

#include "paint_layer_retention.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cmath>
#include <cstring>

namespace tessera::ui::paint::detail {
namespace {

struct Hasher {
    std::uint64_t state = 1469598103934665603ull; // FNV-1a offset basis

    void add(std::uint64_t value) {
        state ^= value;
        state *= 1099511628211ull;
        state ^= state >> 29;
    }
    void add(float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        add(static_cast<std::uint64_t>(bits));
    }
    void add(bool value) {
        add(static_cast<std::uint64_t>(value ? 1 : 0));
    }
    void add(const rhi::Rect& rect) {
        add(rect.x);
        add(rect.y);
        add(rect.width);
        add(rect.height);
    }
    void add(const rhi::Mat3& matrix) {
        for (const float value : matrix.m) {
            add(value);
        }
    }
};

} // namespace

std::uint64_t layer_retention_key(const PaintCacheSignature& s, float own_opacity,
                                  const rendering::gpu::LayerFilterChain& filter, bool masked,
                                  std::uint64_t epoch) {
    Hasher h;
    h.add(s.node);
    h.add(s.bounds);
    h.add(s.transform);
    h.add(s.scrollable_x);
    h.add(s.scrollable_y);
    h.add(s.scroll_x);
    h.add(s.scroll_y);
    h.add(s.scrollport_left);
    h.add(s.scrollport_top);
    h.add(s.scrollport_right);
    h.add(s.scrollport_bottom);
    h.add(s.interpolated);
    h.add(s.painted_world);
    h.add(s.frame_active);
    h.add(s.frame_inverse);
    h.add(s.sink_supports_transform);
    h.add(s.paint_rect);
    h.add(s.clip_hash);
    h.add(static_cast<std::uint64_t>(s.clip_depth));
    h.add(static_cast<std::uint64_t>(s.opacity_bits));
    h.add(static_cast<std::uint64_t>(s.metrics_source_id));
    h.add(static_cast<std::uint64_t>(s.device_scale_bits));
    h.add(s.reduce_effects);
    h.add(s.high_contrast);
    h.add(s.lcd_text);
    h.add(s.isolate_groups);
    h.add(s.in_layer);
    h.add(s.glass_max_blur_radius);
    h.add(s.glass_max_backdrop_area);
    h.add(static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.list_ordinal)));
    h.add(s.list_item);
    h.add(own_opacity);
    h.add(masked);
    h.add(static_cast<std::uint64_t>(filter.count));
    for (std::size_t index = 0; index < filter.count; ++index) {
        const rendering::gpu::LayerFilterOp& op = filter.ops[index];
        h.add(op.offset_x);
        h.add(op.offset_y);
        h.add(op.sigma);
        h.add(static_cast<std::uint64_t>(op.rgba));
        h.add(static_cast<std::uint64_t>(op.kind));
    }
    h.add(epoch);
    return h.state;
}

rhi::Rect retained_layer_box(const Node& node, const rhi::Rect& rect) {
    const rhi::Rect local = node.subtree_paint_bounds();
    if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.width) ||
        !std::isfinite(local.height) || local.width <= 0.0f || local.height <= 0.0f) {
        constexpr float kReach = 1.0e6f;
        return rhi::Rect{-kReach, -kReach, 2.0f * kReach, 2.0f * kReach};
    }
    return rhi::Rect{rect.x + local.x, rect.y + local.y, local.width, local.height};
}

namespace {

// A container worth tracking: it has children (a leaf is already one draw) and a real box.
bool promotion_shape(const Node& node, const rhi::Rect& rect) {
    return !node.children().empty() && rect.width > 0.0f && rect.height > 0.0f;
}

} // namespace

bool promote_now(const PassState& pass, const Node& node, const rhi::Rect& rect,
                 float inherited_opacity, bool in_layer) {
    return pass.promotion != nullptr && pass.retain_layers && !pass.lcd_text && !in_layer &&
           inherited_opacity == 1.0f && promotion_shape(node, rect) &&
           node.dirty() == Dirty::Clean && node.subtree_dirty() == Dirty::Clean &&
           pass.promotion->promotes(node.id(), pass.retain_epoch);
}

void observe_promotion(const PassState& pass, PaintSink& sink, const Node& node,
                       float inherited_opacity, bool in_layer, bool held, const PaintStats& before,
                       const PaintStats& after) {
    const rhi::Rect box = node.bounds();
    if (pass.promotion == nullptr || !pass.retain_layers || pass.lcd_text || in_layer ||
        inherited_opacity != 1.0f || !promotion_shape(node, box) ||
        (held && after.retained_layers != before.retained_layers)) {
        return;
    }
    const PaintCounts counts = paint_counts_since(after, before);
    LayerPromotionObservation observation;
    observation.draws = static_cast<std::uint32_t>(
        counts.backgrounds + counts.borders + counts.texts + counts.vectors + counts.images +
        counts.gradients + counts.shadows + counts.text_shadows + counts.backdrops);
    observation.area = box.width * box.height;
    observation.backdrop = counts.backdrops != 0;
    observation.nested_layer = after.layers != before.layers;
    observation.clean = node.dirty() == Dirty::Clean && node.subtree_dirty() == Dirty::Clean;
    observation.epoch = pass.retain_epoch;
    if (pass.promotion->observe(node.id(), observation)) {
        sink.release_layer(node.id());
    }
}

std::uint64_t layer_key_epoch(const PassState& pass, const Node& node, bool promoted) {
    return promoted ? pass.promotion->key_epoch(node.id(), pass.retain_epoch) : pass.retain_epoch;
}

bool push_layer_retained(PaintSink& sink, const Node& node, const PaintCacheSignature& signature,
                         const LayerDraw& layer, bool masked, std::uint64_t epoch) {
    const bool clean = node.dirty() == Dirty::Clean && node.subtree_dirty() == Dirty::Clean;
    const bool kept = sink.retain_layer(LayerRetention{
        node.id(), layer_retention_key(signature, layer.opacity, layer.filter, masked, epoch),
        clean});
    sink.push_layer(layer);
    return kept;
}

} // namespace tessera::ui::paint::detail
