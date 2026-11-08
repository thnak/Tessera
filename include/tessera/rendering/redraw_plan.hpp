// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"

#include <cstddef>
#include <vector>

namespace tessera::rendering {

struct RedrawPlan {
    bool full_redraw = false;
    std::vector<rhi::Scissor> scissors;
};

struct RedrawPolicy {
    float full_redraw_area_ratio = 0.6f;
    std::size_t max_scissors = 8;
};

RedrawPlan make_redraw_plan(const DamageAccumulator& damage);
bool should_full_redraw(const RedrawPlan& plan, rhi::Extent2D target,
                        const RedrawPolicy& policy = {});

} // namespace tessera::rendering
