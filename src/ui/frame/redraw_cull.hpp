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

// Nullopt means "do not cull": a plan with no scissors (a partial one paints nothing, and a full
// one that carries none, as a hand-built `RedrawPlan{true, {}}` does, has no target to bound it),
// or a scale that cannot map device pixels back to the scene.
//
// A full-redraw plan culls too (issue #257). Every producer of a full plan (`make_redraw_plan`, the
// pipeline's threshold and overflow promotions) stores one scissor covering the whole target, so
// the bounding box is the viewport: a node that cannot reach the target contributes no pixel to a
// full repaint either, and skipping it is what turns a 12 000-node paint from a walk of the whole
// tree into a walk of the visible part. The sink stays unclipped for a full plan, which is why the
// painted output inside the viewport is unchanged.
inline std::optional<rendering::rhi::Rect> plan_cull_rect(const rendering::RedrawPlan& plan,
                                                          float dpi_scale) {
    if (plan.scissors.empty() || !std::isfinite(dpi_scale) || dpi_scale <= 0.0f) {
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
