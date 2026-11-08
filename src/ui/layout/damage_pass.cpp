// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/layout/pixel_snap.hpp"

namespace tessera::ui::layout {
namespace {

constexpr Dirty kDamageDomain = Dirty::Paint | Dirty::Render;

} // namespace

DamagePassResult add_scene_damage(const DirtyPass& dirty, rendering::DamageAccumulator& damage,
                                  float dpi_scale, float margin) {
    DamagePassResult result;
    result.overflowed = dirty.overflowed();

    for (const DirtyNode& entry : dirty.nodes()) {
        if (entry.node == nullptr || !has_any(entry.flags, kDamageDomain)) {
            continue;
        }

        const rendering::rhi::Rect bounds = snap_rect(entry.node->world_bounds(), dpi_scale);
        if (bounds.width <= 0.0f || bounds.height <= 0.0f) {
            continue;
        }

        damage.add(bounds, margin);
        ++result.nodes;
    }

    result.rects = damage.rects().size();
    result.full_redraw = damage.full_redraw();
    return result;
}

} // namespace tessera::ui::layout
