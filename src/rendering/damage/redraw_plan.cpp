// SPDX-License-Identifier: MIT

#include "tessera/rendering/redraw_plan.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tessera::rendering {
namespace {

rhi::Scissor to_scissor(const rhi::Rect& rect, rhi::Extent2D target) {
    const float target_width = static_cast<float>(target.width);
    const float target_height = static_cast<float>(target.height);

    const float left = std::min(std::max(0.0f, rect.x), target_width);
    const float top = std::min(std::max(0.0f, rect.y), target_height);
    const float right = std::min(std::max(0.0f, rect.right()), target_width);
    const float bottom = std::min(std::max(0.0f, rect.bottom()), target_height);

    const std::int32_t x0 = static_cast<std::int32_t>(std::floor(left));
    const std::int32_t y0 = static_cast<std::int32_t>(std::floor(top));
    const std::int32_t x1 = static_cast<std::int32_t>(std::ceil(right));
    const std::int32_t y1 = static_cast<std::int32_t>(std::ceil(bottom));
    if (x1 <= x0 || y1 <= y0) {
        return {};
    }
    return rhi::Scissor{x0, y0, static_cast<std::uint32_t>(x1 - x0),
                        static_cast<std::uint32_t>(y1 - y0)};
}

} // namespace

RedrawPlan make_redraw_plan(const DamageAccumulator& damage) {
    RedrawPlan plan;
    const rhi::Extent2D target = damage.target();
    if (target.width == 0 || target.height == 0) {
        return plan;
    }
    if (damage.full_redraw()) {
        plan.full_redraw = true;
        plan.scissors.push_back(rhi::Scissor{0, 0, target.width, target.height});
        return plan;
    }

    plan.scissors.reserve(damage.rects().size());
    for (const rhi::Rect& rect : damage.rects()) {
        const rhi::Scissor scissor = to_scissor(rect, target);
        if (scissor.width > 0 && scissor.height > 0) {
            plan.scissors.push_back(scissor);
        }
    }
    return plan;
}

bool should_full_redraw(const RedrawPlan& plan, rhi::Extent2D target, const RedrawPolicy& policy) {
    if (plan.full_redraw) {
        return true;
    }
    if (target.width == 0 || target.height == 0 || plan.scissors.empty()) {
        return false;
    }
    if (plan.scissors.size() > policy.max_scissors) {
        return true;
    }

    std::uint64_t area = 0;
    for (const rhi::Scissor& scissor : plan.scissors) {
        area += static_cast<std::uint64_t>(scissor.width) * scissor.height;
    }
    const double target_area = static_cast<double>(target.width) * target.height;
    const double ratio = static_cast<double>(area) / target_area;
    return ratio >= static_cast<double>(policy.full_redraw_area_ratio);
}

} // namespace tessera::rendering
