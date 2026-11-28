// SPDX-License-Identifier: MIT

#include "tessera/ui/capsule/capsule_placement.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui::capsule {

namespace {

constexpr float kEdgeEpsilon = 0.5f;

float finite_or_zero(float value) noexcept {
    return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

bool overlaps_x(const hal::WindowRect& a, float x, float width) noexcept {
    return a.x < x + width && x < a.x + a.width;
}

bool overlaps(const hal::WindowRect& a, const hal::WindowRect& b) noexcept {
    return overlaps_x(a, b.x, b.width) && a.y < b.y + b.height && b.y < a.y + a.height;
}

} // namespace

bool top_cutout(const hal::CutoutInfo& cutouts, float window_width, hal::WindowRect& out) noexcept {
    bool found = false;
    const std::size_t count = std::min<std::size_t>(cutouts.count, cutouts.bounds.size());
    for (std::size_t i = 0; i < count; ++i) {
        const hal::WindowRect& rect = cutouts.bounds[i];
        if (!(rect.width > 0.0f) || !(rect.height > 0.0f)) {
            continue;
        }
        const bool touches_top = rect.y <= kEdgeEpsilon;
        const bool touches_side =
            rect.x <= kEdgeEpsilon || rect.x + rect.width >= window_width - kEdgeEpsilon;
        if (!touches_top || touches_side) {
            continue;
        }
        if (!found || rect.width > out.width) {
            out = rect;
            found = true;
        }
    }
    return found;
}

CapsulePlacement place_capsule(const CapsulePlacementInput& input) noexcept {
    const float window_w = finite_or_zero(input.window_width);
    const float width = finite_or_zero(input.width);
    const float height = finite_or_zero(input.height);

    CapsulePlacement result;
    hal::WindowRect cutout;
    float x = 0.0f;
    float y = 0.0f;
    if (top_cutout(input.cutouts, window_w, cutout)) {
        result.on_cutout = true;
        x = cutout.x + cutout.width * 0.5f - width * 0.5f;
        y = std::max(0.0f, cutout.y + cutout.height * 0.5f - height * 0.5f);
    } else {
        x = (window_w - width) * 0.5f;
        y = finite_or_zero(input.top_inset) + kFallbackTopGap;
    }

    // A hinge that overlaps the capsule moves it to the larger pane; it never sits under the hinge.
    const hal::WindowRect& hinge = input.hinge;
    if (hinge.width > 0.0f && hinge.height > 0.0f &&
        overlaps(hinge, hal::WindowRect{x, y, width, height})) {
        const bool vertical = hinge.height >= hinge.width;
        if (vertical) {
            const float left = std::max(0.0f, hinge.x);
            const float right_start = hinge.x + hinge.width;
            const float right = std::max(0.0f, window_w - right_start);
            const float pane_start = left >= right ? 0.0f : right_start;
            const float pane_width = std::max(left, right);
            x = pane_start + (pane_width - width) * 0.5f;
        } else {
            y = hinge.y + hinge.height + kFallbackTopGap;
        }
        result.moved_for_hinge = true;
    }

    x = std::clamp(x, 0.0f, std::max(0.0f, window_w - width));
    result.rect = {x, y, width, height};
    return result;
}

} // namespace tessera::ui::capsule
