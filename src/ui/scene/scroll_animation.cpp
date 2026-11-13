// SPDX-License-Identifier: MIT
//
// Smooth programmatic scrolling (`scroll-behavior: smooth`, docs/specs/scroll-and-gestures-v1.md
// section 2). A request stores a clamped target; each fixed step moves the offset by a
// deterministic exponential approach and the final sub-epsilon step snaps. Allocation-free,
// UI-thread only, no wall clock: the frame loop passes the tick duration, so the same dt sequence
// always produces the same offsets.

#include "tessera/ui/scene/scroll.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace tessera::ui {
namespace {

float finite(float value) {
    return std::isfinite(value) ? value : 0.0f;
}

float clamped_x(const Node& node, float x) {
    return std::clamp(finite(x), 0.0f, max_scroll_x(node));
}

float clamped_y(const Node& node, float y) {
    return std::clamp(finite(y), 0.0f, max_scroll_y(node));
}

} // namespace

bool scroll_to_programmatic(Node& node, float x, float y, bool reduced_motion) {
    const float target_x = clamped_x(node, x);
    const float target_y = clamped_y(node, y);
    // Reduced motion makes every smooth request immediate (docs/specs/accessibility-v1.md).
    const bool smooth = node.scroll_behavior() == ScrollBehavior::Smooth && !reduced_motion;
    if (!smooth) {
        node.clear_scroll_animation();
        return scroll_to(node, target_x, target_y);
    }
    if (target_x == node.scroll_x() && target_y == node.scroll_y()) {
        // Requesting the current offset cancels an in-flight animation instead of starting a no-op.
        node.clear_scroll_animation();
        return false;
    }
    node.set_scroll_animation_target(target_x, target_y);
    return true;
}

bool scroll_by_programmatic(Node& node, float dx, float dy, bool reduced_motion) {
    return scroll_to_programmatic(node, node.scroll_x() + finite(dx), node.scroll_y() + finite(dy),
                                  reduced_motion);
}

bool advance_scroll_animation(Node& node, double dt, bool reduced_motion) {
    if (!node.scroll_animation_active()) {
        return false;
    }
    // Content/viewport changes since the request can lower the maximum offset; re-clamp the target
    // before stepping so the invariant (offset <= max) always holds.
    const float target_x = clamped_x(node, node.scroll_animation_target_x());
    const float target_y = clamped_y(node, node.scroll_animation_target_y());
    if (reduced_motion) {
        node.clear_scroll_animation();
        (void)scroll_to(node, target_x, target_y);
        return false;
    }
    if (!std::isfinite(dt) || dt <= 0.0) {
        return true;
    }
    node.set_scroll_animation_target(target_x, target_y);
    const float factor = std::exp(-static_cast<float>(dt) / kSmoothScrollTimeConstant);
    float next_x = target_x + (node.scroll_x() - target_x) * factor;
    float next_y = target_y + (node.scroll_y() - target_y) * factor;
    const bool settled = std::abs(target_x - next_x) <= kSmoothScrollEpsilon &&
                         std::abs(target_y - next_y) <= kSmoothScrollEpsilon;
    if (settled) {
        next_x = target_x;
        next_y = target_y;
        node.clear_scroll_animation();
    }
    // scroll_to clamps again and emits exactly one Scroll event for the applied move.
    (void)scroll_to(node, next_x, next_y);
    return !settled;
}

std::size_t advance_scroll_animations(Node& root, double dt, bool reduced_motion) {
    std::size_t animating = advance_scroll_animation(root, dt, reduced_motion) ? 1 : 0;
    for (const auto& child : root.children()) {
        animating += advance_scroll_animations(*child, dt, reduced_motion);
    }
    return animating;
}

} // namespace tessera::ui
