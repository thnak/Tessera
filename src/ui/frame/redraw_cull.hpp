// SPDX-License-Identifier: MIT
//
// The world-space cull rectangle the frame pipeline hands to the paint pass (issue #487): the
// bounding box of the redraw plan's scissors, in device pixels, divided by the DPI scale back into
// the scene's logical coordinates (`Node::world_bounds()` space). Kept out of `frame_pipeline.cpp`,
// which is at its line limit, and separate from the paint pass's own `paint_cull.hpp`, which must
// not learn about redraw plans.

#pragma once

#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace tessera::ui::frame::detail {

// Nullopt means "do not cull": a full-redraw plan (its scissor list is ignored and the sink paints
// unclipped), an empty partial plan (nothing is painted anyway), or a scale that cannot map device
// pixels back to the scene.
inline std::optional<rendering::rhi::Rect> plan_cull_rect(const rendering::RedrawPlan& plan,
                                                          float dpi_scale) {
    if (plan.full_redraw || plan.scissors.empty() || !std::isfinite(dpi_scale) ||
        dpi_scale <= 0.0f) {
        return std::nullopt;
    }
    std::int64_t left = std::numeric_limits<std::int64_t>::max();
    std::int64_t top = left;
    std::int64_t right = std::numeric_limits<std::int64_t>::min();
    std::int64_t bottom = right;
    for (const rendering::rhi::Scissor& scissor : plan.scissors) {
        left = std::min<std::int64_t>(left, scissor.x);
        top = std::min<std::int64_t>(top, scissor.y);
        right = std::max<std::int64_t>(right, std::int64_t{scissor.x} + scissor.width);
        bottom = std::max<std::int64_t>(bottom, std::int64_t{scissor.y} + scissor.height);
    }
    if (right <= left || bottom <= top) {
        return std::nullopt;
    }
    const float scale = 1.0f / dpi_scale;
    return rendering::rhi::Rect{static_cast<float>(left) * scale, static_cast<float>(top) * scale,
                                static_cast<float>(right - left) * scale,
                                static_cast<float>(bottom - top) * scale};
}

} // namespace tessera::ui::frame::detail
