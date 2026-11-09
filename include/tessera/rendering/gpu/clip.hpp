// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tessera::rendering::gpu::clip {

// Clip model for the batched 2D renderer: an axis-aligned rect plus four per-corner radii (TL, TR,
// BR, BL), clamped to half the smaller dimension. A clip with all radii zero is a plain rectangle
// and uses the scissor fast path; any non-zero radius switches that draw to the analytic fragment
// test. `kMaxClipDepth` bounds the clip stack; `kMaxShaderClips` is the number of folded clips the
// fragment shader receives as push constants (deeper clips are folded conservatively).
inline constexpr std::uint32_t kMaxClipDepth = 8;
inline constexpr std::uint32_t kMaxShaderClips = 3;

struct RoundedRect {
    rhi::Rect rect;
    float top_left = 0.0f;
    float top_right = 0.0f;
    float bottom_right = 0.0f;
    float bottom_left = 0.0f;

    [[nodiscard]] bool has_area() const {
        return rect.width > 0.0f && rect.height > 0.0f;
    }
    // True when any corner is rounded (the shader path is needed).
    [[nodiscard]] bool rounded() const {
        return top_left > 0.0f || top_right > 0.0f || bottom_right > 0.0f || bottom_left > 0.0f;
    }
};

// Clamps every radius into [0, min(width, height) / 2], keeping the clip representable by the
// per-corner rounded-rect test (and matching CSS radius clamping).
[[nodiscard]] inline RoundedRect clamp_radii(RoundedRect clip) {
    const float limit = std::min(clip.rect.width, clip.rect.height) * 0.5f;
    const auto limit_radius = [limit](float radius) {
        return std::clamp(radius, 0.0f, std::max(limit, 0.0f));
    };
    clip.top_left = limit_radius(clip.top_left);
    clip.top_right = limit_radius(clip.top_right);
    clip.bottom_right = limit_radius(clip.bottom_right);
    clip.bottom_left = limit_radius(clip.bottom_left);
    return clip;
}

[[nodiscard]] inline RoundedRect make_rounded_rect(const rhi::Rect& rect, float top_left,
                                                   float top_right, float bottom_right,
                                                   float bottom_left) {
    RoundedRect clip;
    clip.rect = rect;
    clip.top_left = top_left;
    clip.top_right = top_right;
    clip.bottom_right = bottom_right;
    clip.bottom_left = bottom_left;
    return clamp_radii(clip);
}

[[nodiscard]] inline RoundedRect uniform_rect(const rhi::Rect& rect, float radius) {
    return make_rounded_rect(rect, radius, radius, radius, radius);
}

// Conservative nesting intersection. Exact analytic intersection of two rounded rects is not
// needed for V1; this rule is exact for containment and never paints outside either clip:
//
//   rect = intersection of the two rects.
//   For each corner, the radius is the largest radius of a clip whose *both* edges coincide with
//   that intersection corner; corners created by a straight edge cut get radius 0 (square cut).
//
// Consequences: rounded-in-rect keeps the rounded corners that survive and squares the cut corners;
// rounded-in-rounded takes the tighter (larger) radius where both corners coincide. A larger radius
// is locally a subset of a smaller one, so the result stays inside both clips; at corners created
// by a cut the straight result may be slightly sharper than the true analytic intersection (sound
// under-paint, never over-paint). The result is clamped again, so it is always representable.
[[nodiscard]] inline RoundedRect intersect_clips(const RoundedRect& outer,
                                                 const RoundedRect& inner) {
    const float x0 = std::max(outer.rect.x, inner.rect.x);
    const float y0 = std::max(outer.rect.y, inner.rect.y);
    const float x1 = std::min(outer.rect.right(), inner.rect.right());
    const float y1 = std::min(outer.rect.bottom(), inner.rect.bottom());
    if (x1 <= x0 || y1 <= y0) {
        return RoundedRect{};
    }
    RoundedRect result;
    result.rect = rhi::Rect{x0, y0, x1 - x0, y1 - y0};

    // A corner contributes its radius only when it is the same corner in both clips; otherwise the
    // other clip cut it with a straight edge.
    const auto same_x = [](float a, float b) { return std::abs(a - b) <= 1.0e-4f; };
    const auto same_y = [](float a, float b) { return std::abs(a - b) <= 1.0e-4f; };
    const auto corner_radius = [&](float x, float y, bool right_edge, bool bottom_edge,
                                   auto radius_of) {
        float radius = 0.0f;
        for (const RoundedRect* source : {&outer, &inner}) {
            const float edge_x = right_edge ? source->rect.right() : source->rect.x;
            const float edge_y = bottom_edge ? source->rect.bottom() : source->rect.y;
            if (same_x(edge_x, x) && same_y(edge_y, y)) {
                radius = std::max(radius, radius_of(*source));
            }
        }
        return radius;
    };
    result.top_left =
        corner_radius(x0, y0, false, false, [](const RoundedRect& c) { return c.top_left; });
    result.top_right =
        corner_radius(x1, y0, true, false, [](const RoundedRect& c) { return c.top_right; });
    result.bottom_right =
        corner_radius(x1, y1, true, true, [](const RoundedRect& c) { return c.bottom_right; });
    result.bottom_left =
        corner_radius(x0, y1, false, true, [](const RoundedRect& c) { return c.bottom_left; });
    return clamp_radii(result);
}

// Smallest integer scissor containing the clip rect (conservative for the scissor fast path; the
// shader still tests the exact fractional rect for rounded clips).
[[nodiscard]] inline rhi::Scissor enclosing_scissor(const RoundedRect& clip) {
    if (!clip.has_area()) {
        return rhi::Scissor{};
    }
    const auto x0 = static_cast<std::int32_t>(std::floor(clip.rect.x));
    const auto y0 = static_cast<std::int32_t>(std::floor(clip.rect.y));
    const auto x1 = static_cast<std::int32_t>(std::ceil(clip.rect.right()));
    const auto y1 = static_cast<std::int32_t>(std::ceil(clip.rect.bottom()));
    if (x1 <= x0 || y1 <= y0) {
        return rhi::Scissor{};
    }
    return rhi::Scissor{x0, y0, static_cast<std::uint32_t>(x1 - x0),
                        static_cast<std::uint32_t>(y1 - y0)};
}

[[nodiscard]] inline bool same_rect(const rhi::Rect& a, const rhi::Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

[[nodiscard]] inline bool operator==(const RoundedRect& a, const RoundedRect& b) {
    return same_rect(a.rect, b.rect) && a.top_left == b.top_left && a.top_right == b.top_right &&
           a.bottom_right == b.bottom_right && a.bottom_left == b.bottom_left;
}

} // namespace tessera::rendering::gpu::clip
