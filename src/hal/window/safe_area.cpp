// SPDX-License-Identifier: MIT

#include "tessera/hal/safe_area.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::hal {

namespace {

constexpr float kAbutTolerance = 1.0f;

float clean(float value) noexcept {
    return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

WindowInsets cleaned(const WindowInsets& insets) noexcept {
    return {clean(insets.left), clean(insets.top), clean(insets.right), clean(insets.bottom)};
}

void raise(WindowInsets& into, const WindowInsets& other) noexcept {
    into.left = std::max(into.left, other.left);
    into.top = std::max(into.top, other.top);
    into.right = std::max(into.right, other.right);
    into.bottom = std::max(into.bottom, other.bottom);
}

} // namespace

WindowInsets cutout_insets(const WindowMetrics& metrics) noexcept {
    WindowInsets out = cleaned(metrics.cutouts.waterfall_insets);
    const float width = static_cast<float>(metrics.logical_width);
    const float height = static_cast<float>(metrics.logical_height);
    const std::size_t count = std::min<std::size_t>(metrics.cutouts.count, kMaxCutouts);
    for (std::size_t i = 0; i < count; ++i) {
        const WindowRect& rect = metrics.cutouts.bounds[i];
        const float right = rect.x + rect.width;
        const float bottom = rect.y + rect.height;
        if (rect.y <= kAbutTolerance) {
            out.top = std::max(out.top, clean(bottom));
        }
        if (bottom >= height - kAbutTolerance) {
            out.bottom = std::max(out.bottom, clean(height - rect.y));
        }
        if (rect.x <= kAbutTolerance) {
            out.left = std::max(out.left, clean(right));
        }
        if (right >= width - kAbutTolerance) {
            out.right = std::max(out.right, clean(width - rect.x));
        }
    }
    return out;
}

WindowInsets hinge_insets(const WindowMetrics& metrics) noexcept {
    WindowInsets out;
    const FoldInfo& fold = metrics.fold;
    if (fold.occlusion != FoldOcclusion::Full) {
        return out;
    }
    const WindowRect& hinge = fold.hinge_bounds;
    const float width = static_cast<float>(metrics.logical_width);
    const float height = static_cast<float>(metrics.logical_height);
    if (fold.orientation == FoldOrientation::Vertical) {
        const float before = clean(hinge.x);
        const float after = clean(width - (hinge.x + hinge.width));
        if (before >= after) {
            out.right = clean(width - hinge.x);
        } else {
            out.left = clean(hinge.x + hinge.width);
        }
    } else if (fold.orientation == FoldOrientation::Horizontal) {
        const float before = clean(hinge.y);
        const float after = clean(height - (hinge.y + hinge.height));
        if (before >= after) {
            out.bottom = clean(height - hinge.y);
        } else {
            out.top = clean(hinge.y + hinge.height);
        }
    }
    return out;
}

WindowInsets resolve_safe_area(const WindowMetrics& metrics,
                               const SafeAreaConfig& config) noexcept {
    WindowInsets out;
    if ((config.sources & kSourceSystemBars) != 0) {
        raise(out, cleaned(metrics.insets));
    }
    if ((config.sources & kSourceCutout) != 0) {
        raise(out, cutout_insets(metrics));
    }
    if ((config.sources & kSourceIme) != 0) {
        raise(out, cleaned(metrics.ime_insets));
    }
    if ((config.sources & kSourceGestures) != 0) {
        raise(out, cleaned(metrics.gesture_insets));
    }
    if ((config.sources & kSourceHinge) != 0) {
        raise(out, hinge_insets(metrics));
    }

    std::uint8_t mask = config.edges;
    const std::uint8_t start = config.rtl ? kEdgeRight : kEdgeLeft;
    const std::uint8_t end = config.rtl ? kEdgeLeft : kEdgeRight;
    if ((mask & kEdgeStart) != 0) {
        mask = static_cast<std::uint8_t>(mask | start);
    }
    if ((mask & kEdgeEnd) != 0) {
        mask = static_cast<std::uint8_t>(mask | end);
    }

    const float floor = clean(config.minimum);
    const auto side = [&](std::uint8_t bit, float value) noexcept {
        return (mask & bit) != 0 ? std::max(value, floor) : 0.0f;
    };
    return {side(kEdgeLeft, out.left), side(kEdgeTop, out.top), side(kEdgeRight, out.right),
            side(kEdgeBottom, out.bottom)};
}

} // namespace tessera::hal
