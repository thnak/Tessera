// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"

#include <vector>

namespace tessera::rendering {

struct RedrawPlan {
    bool full_redraw = false;
    std::vector<rhi::Scissor> scissors;
};

RedrawPlan make_redraw_plan(const DamageAccumulator& damage);

} // namespace tessera::rendering
