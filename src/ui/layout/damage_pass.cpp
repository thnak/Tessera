// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/layout/pixel_snap.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui::layout {
namespace {

constexpr Dirty kDamageDomain = Dirty::Paint | Dirty::Render;

} // namespace

// Text damages its whole box. The damage layer knows neither the computed padding (paint offsets
// the glyph run by it and wraps at the padded width) nor the real font metrics (paint uses the
// context metrics source, not the stub), so any line-union estimate can fall short of the pixels
// actually drawn and leave stale glyph rows. Tightening needs the paint pass's content box and
// metrics source.
rendering::rhi::Rect node_damage_rect(const Node& node) {
    rendering::rhi::Rect bounds = node.world_bounds();
    const Node::PaintOutsets& outsets = node.paint_outsets();
    if (outsets.left <= 0.0f && outsets.top <= 0.0f && outsets.right <= 0.0f &&
        outsets.bottom <= 0.0f) {
        return bounds;
    }
    // Outsets are box-local px; the world transform scales them (the largest axis scale keeps the
    // rect conservative under non-uniform scale and rotation).
    const rendering::rhi::Mat3 world = node.world_transform();
    const float scale =
        std::max({std::hypot(world.m[0], world.m[3]), std::hypot(world.m[1], world.m[4]), 0.0f});
    bounds.x -= outsets.left * scale;
    bounds.y -= outsets.top * scale;
    bounds.width += (outsets.left + outsets.right) * scale;
    bounds.height += (outsets.top + outsets.bottom) * scale;
    return bounds;
}

rendering::rhi::Rect node_glass_dependency_rect(const Node& node, float dpi_scale) {
    const float sigma = node.glass_blur_sigma();
    if (sigma <= 0.0f) {
        return {};
    }
    // Box grown by 3 sigma in presented (CSS) pixels, snapped to the device grid like every other
    // damage rect. The world box already carries transforms; the region grows by the sigma the
    // paint pass emits, so a dependency test here matches the blur the sink will run.
    const rendering::rhi::Rect region =
        rendering::gpu::glass_dependency_rect(node.world_bounds(), sigma, 1.0f);
    return snap_rect(region, dpi_scale);
}

DamagePassResult add_scene_damage(const DirtyPass& dirty, rendering::DamageAccumulator& damage,
                                  float dpi_scale, float margin) {
    DamagePassResult result;
    result.overflowed = dirty.overflowed();

    for (const DirtyNode& entry : dirty.nodes()) {
        if (entry.node == nullptr || !has_any(entry.flags, kDamageDomain)) {
            continue;
        }

        const rendering::rhi::Rect bounds = snap_rect(node_damage_rect(*entry.node), dpi_scale);
        if (bounds.width <= 0.0f || bounds.height <= 0.0f) {
            continue;
        }

        damage.add(bounds, margin);
        ++result.nodes;
    }

    // A truncated dirty list cannot prove the damage is complete: the missing nodes may have
    // changed pixels anywhere in their (unknown) bounds. Over-damage is cheap and safe;
    // under-damage leaves stale pixels, so an overflow forces a full-target redraw.
    if (dirty.overflowed()) {
        damage.force_full_redraw();
    }
    result.rects = damage.rects().size();
    result.full_redraw = damage.full_redraw();
    return result;
}

} // namespace tessera::ui::layout
