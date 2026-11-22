// SPDX-License-Identifier: MIT

// Device-space geometry for the paint pass: a transformed node paints in its own local frame.
//
// These were the opening of `paint_pass.cpp` and are separated because they are pure -- a matrix, a
// rect and a clip in, geometry out, with no paint sink and no computed style -- so a reader meets
// "how a local rect becomes device space" before the painting functions that assume it.
// `paint_pass.cpp` reached the 500-line hard limit when the rounded-rect outline primitive joined
// the box and border path, and splitting by responsibility is the standing rule for that.

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/node_fwd.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace tessera::ui::paint::detail {

// A transformed node (CSS `transform`) is painted in its own local frame when the sink can apply a
// matrix: the sink holds `world` (the node's full world matrix) and every rect in the subtree is
// expressed relative to it. `active` is false for plain scenes, which keep painting world bounds.
struct Frame {
    bool active = false;
    rhi::Mat3 world;
    rhi::Mat3 inverse;
};

inline bool is_identity(const rhi::Mat3& matrix) {
    const rhi::Mat3 identity;
    for (int index = 0; index < 9; ++index) {
        if (matrix.m[index] != identity.m[index]) {
            return false;
        }
    }
    return true;
}

inline rhi::Rect transformed_aabb(const rhi::Mat3& matrix, const rhi::Rect& rect) {
    float xs[4];
    float ys[4];
    matrix.transform_point(rect.x, rect.y, xs[0], ys[0]);
    matrix.transform_point(rect.right(), rect.y, xs[1], ys[1]);
    matrix.transform_point(rect.right(), rect.bottom(), xs[2], ys[2]);
    matrix.transform_point(rect.x, rect.bottom(), xs[3], ys[3]);
    const auto [min_x, max_x] = std::minmax_element(xs, xs + 4);
    const auto [min_y, max_y] = std::minmax_element(ys, ys + 4);
    return rhi::Rect{*min_x, *min_y, *max_x - *min_x, *max_y - *min_y};
}

// Bounds are parent-relative; Node::world_bounds() composes them into absolute coordinates, so a
// plain pass paints that rect directly (see paint_pass.hpp). Inside a transformed frame the rect is
// the node's box relative to the frame: between a frame and its descendants only translations
// (and scroll offsets) remain, so this is exact.
//
// `painted_world` is the matrix the node paints with and `interpolated` says whether an
// interpolation view moved it away from Node::world_transform(). When it did not, the authoritative
// accessors are used verbatim so a non-interpolated frame is byte-identical to before the view
// existed.
inline rhi::Rect paint_rect(const Node& node, const Frame& frame, bool interpolated,
                            const rhi::Mat3& painted_world) {
    if (!frame.active) {
        if (!interpolated) {
            return node.world_bounds();
        }
        return transformed_aabb(painted_world,
                                rhi::Rect{0.0f, 0.0f, node.bounds().width, node.bounds().height});
    }
    const rhi::Mat3 relative = frame.inverse * painted_world;
    return transformed_aabb(relative,
                            rhi::Rect{0.0f, 0.0f, node.bounds().width, node.bounds().height});
}

// Clips are device-space state: inside a transformed frame a local clip is mapped through the
// frame. Scale/translate/quarter-turn frames map a rect to a rect; any other frame cannot be
// represented by a rounded rect, so its overflow clip is dropped rather than painted wrong.
inline std::optional<RoundedClip> device_clip(const RoundedClip& clip, const Frame& frame) {
    if (!frame.active) {
        return clip;
    }
    const rhi::Mat3& m = frame.world;
    constexpr float kEpsilon = 1.0e-4f;
    const bool axis_scale = std::abs(m.m[1]) < kEpsilon && std::abs(m.m[3]) < kEpsilon;
    const bool quarter_turn = std::abs(m.m[0]) < kEpsilon && std::abs(m.m[4]) < kEpsilon;
    if (!axis_scale && !quarter_turn) {
        return std::nullopt;
    }
    const float scale = std::sqrt(std::abs(m.m[0] * m.m[4] - m.m[1] * m.m[3]));
    RoundedClip mapped;
    mapped.rect = transformed_aabb(m, clip.rect);
    mapped.top_left = clip.top_left * scale;
    mapped.top_right = clip.top_right * scale;
    mapped.bottom_right = clip.bottom_right * scale;
    mapped.bottom_left = clip.bottom_left * scale;
    return mapped;
}

inline float clamp_radius(float radius, const rhi::Rect& rect) {
    return std::min({radius, rect.width * 0.5f, rect.height * 0.5f});
}

} // namespace tessera::ui::paint::detail
