// SPDX-License-Identifier: MIT

#include "tessera/ui/overlay/placement.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui {

namespace {

using rendering::rhi::Rect;

enum class Edge : std::uint8_t { Top, Bottom, Left, Right, Center };

constexpr float kEpsilon = 0.01f;

float finite(float value) noexcept {
    return std::isfinite(value) ? value : 0.0f;
}

float extent(float value) noexcept {
    return std::max(0.0f, finite(value));
}

Rect sanitize(const Rect& rect) noexcept {
    return {finite(rect.x), finite(rect.y), extent(rect.width), extent(rect.height)};
}

Edge physical(OverlaySide side, bool rtl) noexcept {
    switch (side) {
    case OverlaySide::Top:
        return Edge::Top;
    case OverlaySide::Bottom:
        return Edge::Bottom;
    case OverlaySide::Start:
        return rtl ? Edge::Right : Edge::Left;
    case OverlaySide::End:
        return rtl ? Edge::Left : Edge::Right;
    case OverlaySide::Center:
        break;
    }
    return Edge::Center;
}

OverlaySide logical(Edge edge, bool rtl) noexcept {
    switch (edge) {
    case Edge::Top:
        return OverlaySide::Top;
    case Edge::Bottom:
        return OverlaySide::Bottom;
    case Edge::Left:
        return rtl ? OverlaySide::End : OverlaySide::Start;
    case Edge::Right:
        return rtl ? OverlaySide::Start : OverlaySide::End;
    case Edge::Center:
        break;
    }
    return OverlaySide::Center;
}

Edge opposite(Edge edge) noexcept {
    switch (edge) {
    case Edge::Top:
        return Edge::Bottom;
    case Edge::Bottom:
        return Edge::Top;
    case Edge::Left:
        return Edge::Right;
    case Edge::Right:
        return Edge::Left;
    case Edge::Center:
        break;
    }
    return Edge::Center;
}

// Position along the cross axis of `frame` (the anchor, or the viewport for a window anchor).
// `horizontal` is the axis being aligned; start is the left edge, or the right one under RTL.
float aligned(const Rect& frame, float size, bool horizontal, OverlayAlign align,
              bool rtl) noexcept {
    const float origin = horizontal ? frame.x : frame.y;
    const float length = horizontal ? frame.width : frame.height;
    const float far_edge = origin + length - size;
    switch (align) {
    case OverlayAlign::Start:
        return horizontal && rtl ? far_edge : origin;
    case OverlayAlign::End:
        return horizontal && rtl ? origin : far_edge;
    case OverlayAlign::Center:
        break;
    }
    return origin + (length - size) * 0.5f;
}

// The box on one edge. A window anchor places it inside `frame` against that edge; any other
// anchor places it outside, `gap` away.
Rect at_edge(Edge edge, const Rect& frame, bool interior, float width, float height, float gap,
             OverlayAlign align, bool rtl) noexcept {
    Rect box{0.0f, 0.0f, width, height};
    switch (edge) {
    case Edge::Top:
        box.y = interior ? frame.y + gap : frame.y - gap - height;
        box.x = aligned(frame, width, true, align, rtl);
        break;
    case Edge::Bottom:
        box.y = interior ? frame.y + frame.height - gap - height : frame.y + frame.height + gap;
        box.x = aligned(frame, width, true, align, rtl);
        break;
    case Edge::Left:
        box.x = interior ? frame.x + gap : frame.x - gap - width;
        box.y = aligned(frame, height, false, align, rtl);
        break;
    case Edge::Right:
        box.x = interior ? frame.x + frame.width - gap - width : frame.x + frame.width + gap;
        box.y = aligned(frame, height, false, align, rtl);
        break;
    case Edge::Center:
        box.x = aligned(frame, width, true, OverlayAlign::Center, rtl);
        box.y = aligned(frame, height, false, OverlayAlign::Center, rtl);
        break;
    }
    return box;
}

bool overflows(Edge edge, const Rect& box, const Rect& viewport) noexcept {
    switch (edge) {
    case Edge::Top:
        return box.y < viewport.y - kEpsilon;
    case Edge::Bottom:
        return box.y + box.height > viewport.y + viewport.height + kEpsilon;
    case Edge::Left:
        return box.x < viewport.x - kEpsilon;
    case Edge::Right:
        return box.x + box.width > viewport.x + viewport.width + kEpsilon;
    case Edge::Center:
        break;
    }
    return false;
}

// Free space between the anchor and the viewport edge on that side.
float room(Edge edge, const Rect& anchor, const Rect& viewport) noexcept {
    switch (edge) {
    case Edge::Top:
        return anchor.y - viewport.y;
    case Edge::Bottom:
        return viewport.y + viewport.height - (anchor.y + anchor.height);
    case Edge::Left:
        return anchor.x - viewport.x;
    case Edge::Right:
        return viewport.x + viewport.width - (anchor.x + anchor.width);
    case Edge::Center:
        break;
    }
    return 0.0f;
}

float slide(float position, float size, float origin, float length) noexcept {
    if (size >= length) {
        return origin;
    }
    return std::clamp(position, origin, origin + length - size);
}

} // namespace

PlacementResult compute_placement(const PlacementInput& input) noexcept {
    const Rect viewport = sanitize(input.viewport);
    const Rect anchor = input.anchor_is_window ? viewport : sanitize(input.anchor);
    const OverlayPlacement& want = input.placement;
    const float width = extent(input.width);
    const float height = extent(input.height);
    const float gap = finite(want.gap);
    const bool interior = input.anchor_is_window;

    Edge edge = physical(want.side, input.rtl);
    Rect box = at_edge(edge, anchor, interior, width, height, gap, want.align, input.rtl);

    PlacementResult result;
    // A window anchor already sits against its own edge, and Center has no side to flip.
    if (want.flip && !interior && edge != Edge::Center && overflows(edge, box, viewport)) {
        const Edge other = opposite(edge);
        const Rect flipped =
            at_edge(other, anchor, interior, width, height, gap, want.align, input.rtl);
        if (!overflows(other, flipped, viewport) ||
            room(other, anchor, viewport) > room(edge, anchor, viewport)) {
            edge = other;
            box = flipped;
            result.flipped = true;
        }
    }

    if (want.shift) {
        const Rect before = box;
        box.x = slide(box.x, width, viewport.x, viewport.width);
        box.y = slide(box.y, height, viewport.y, viewport.height);
        result.shifted =
            std::fabs(box.x - before.x) > kEpsilon || std::fabs(box.y - before.y) > kEpsilon;
    }
    result.rect = box;
    result.side = logical(edge, input.rtl);
    return result;
}

} // namespace tessera::ui
