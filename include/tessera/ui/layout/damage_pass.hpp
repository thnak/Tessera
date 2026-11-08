// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"

#include <cstddef>

namespace tessera::ui::layout {

struct DamagePassResult {
    std::size_t nodes = 0;
    std::size_t rects = 0;
    bool full_redraw = false;
    bool overflowed = false;
};

DamagePassResult add_scene_damage(const DirtyPass& dirty, rendering::DamageAccumulator& damage,
                                  float dpi_scale = 1.0f, float margin = 0.0f);

} // namespace tessera::ui::layout
