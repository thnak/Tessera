// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/layout/pixel_snap.hpp"

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
    return node.world_bounds();
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
