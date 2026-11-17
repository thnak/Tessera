// SPDX-License-Identifier: MIT

// Paint-pass culling (issue #487): the reach test and the skip decision that let `paint_node` avoid
// a style resolution and an emission for a node whose pixels cannot reach the target. Split out of
// `paint_pass.cpp` for the same two reasons as `paint_frame.hpp`: it is pure - a node, a matrix and
// a rect in, a verdict out, no sink and no computed style required - and the paint loop itself is
// at its line limit.
//
// The contract `paint_node` relies on: a verdict other than `Paint` is only returned for a node
// whose entire contribution to the target - its own draws and, when `Subtree`, every draw of its
// descendants - lies outside `PaintContext::cull`. The rules that make that true are the ones this
// file encodes:
//   - the reach is a superset of what the node paints (border box + box-shadow outsets + 3 glass
//     sigmas + a pixel of antialiasing/pixel-snap slack), mapped through the matrix it paints with;
//   - a text payload can paint outside its own box (wrapped lines advance past the content box), so
//     a node that paints text is only ever culled as a `Subtree`, and only when the pass clips it;
//   - children are only bounded by an actual clip push (overflow or scroll), never by the box: a
//     non-clipping parent's children may overflow anywhere, so its own emission is skipped but its
//     children are still walked (`Own`).
// A node painted in its own transform frame never reaches this decision (the caller refuses it).

#pragma once

#include "paint_frame.hpp"
#include "paint_properties.hpp"
#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace tessera::ui::paint::detail {

enum class CullVerdict {
    Paint,   // nothing can be skipped: the node's own content may reach the target
    Node,    // a leaf whose own emission is inside the reach: skip the whole node
    Own,     // skip the node's own emission, walk its children (a non-clipping parent)
    Subtree, // the pass clips the children inside the reach: skip the whole subtree
};

// Outsets are a paint-side hint that a caller can set directly, so a negative or non-finite value
// must not be allowed to shrink the reach below the box (a negative outset still paints inside the
// box, and a shadow it describes cannot leave it).
inline float reach_outsets(float value) {
    return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

// True when the node's painted reach can touch `cull`. `painted_world` is the matrix the node
// paints with (between a frame and its descendants only translations remain, so the local box maps
// exactly); `kEdge` covers antialiasing and the pixel snapping of border widths.
inline bool reaches_cull(const Node& node, const rhi::Mat3& painted_world, const rhi::Rect& cull) {
    const Node::PaintOutsets& outsets = node.paint_outsets();
    const float sigma = std::isfinite(node.glass_blur_sigma()) ? node.glass_blur_sigma() : 0.0f;
    const float glass = 3.0f * std::max(0.0f, sigma);
    constexpr float kEdge = 1.0f;
    const float left = reach_outsets(outsets.left) + glass + kEdge;
    const float top = reach_outsets(outsets.top) + glass + kEdge;
    const float right = reach_outsets(outsets.right) + glass + kEdge;
    const float bottom = reach_outsets(outsets.bottom) + glass + kEdge;
    const rhi::Rect box{-left, -top, node.bounds().width + left + right,
                        node.bounds().height + top + bottom};
    const rhi::Rect reach = transformed_aabb(painted_world, box);
    if (!std::isfinite(reach.x) || !std::isfinite(reach.y) || !std::isfinite(reach.width) ||
        !std::isfinite(reach.height)) {
        return true; // never cull what cannot be placed
    }
    return reach.x < cull.right() && reach.right() > cull.x && reach.y < cull.bottom() &&
           reach.bottom() > cull.y;
}

// True when everything the node paints for itself is inside the reach: box visuals (background,
// border, gradient, backdrop, box shadow) are inside the border box or its synced shadow outsets,
// and the vector and image payloads are placed at the content box. Text is the one payload that can
// leave the box: `paint_text_lines` advances the line origin past the content box for every wrapped
// line it emits and does not clip, so a box test alone cannot bound an unclipped text run.
inline bool own_emission_bounded(const Node& node) {
    const auto& data = node.paint_data();
    if (!data.has_value() || data->kind != PaintKind::Text) {
        return true;
    }
    const bool paints_content =
        !data->text.empty() || !data->spans.empty() ||
        (data->decoration.has_value() && data->decoration->composition_active);
    return !paints_content;
}

// True when the pass pushes a clip around this node's children that lies inside the node's border
// box - the only thing that bounds a child which overflows. Mirrors the emission path exactly,
// including its two "no clip after all" cases: a padding box the border widths consumed, and a
// device clip that a non-axis-aligned ancestor frame cannot express (the pass then drops the clip
// rather than painting it wrong, so children are not bounded either).
inline bool clipped_children(const Node& node, const style::ComputedStyle& style,
                             const rhi::Rect& rect, const Frame& frame) {
    if (rect.width <= 0.0f || rect.height <= 0.0f) {
        return false;
    }
    const bool scroll_clips = node.scrollable_x() || node.scrollable_y();
    if (!clips_children(style) && !scroll_clips) {
        return false;
    }
    const std::optional<rhi::Rect> clip_rect =
        scroll_clips ? scrollport_clip(node, style, rect) : padding_box(style, rect);
    if (!clip_rect.has_value()) {
        return false;
    }
    return device_clip(corner_clip(style, *clip_rect), frame).has_value();
}

// The cull verdict for a node whose reach already missed the rect and that does not paint in its
// own transform frame. `style` is the node's computed style; the caller must provide it when the
// node has children (both the clip decision and the opacity its children inherit need it) and may
// leave it null for a leaf, which is the point: a leaf outside the rect never resolves a style.
inline CullVerdict cull_verdict(const Node& node, const rhi::Rect& rect, const Frame& frame,
                                const style::ComputedStyle* style) {
    const bool bounded = own_emission_bounded(node);
    if (node.children().empty()) {
        return bounded ? CullVerdict::Node : CullVerdict::Paint;
    }
    if (style == nullptr) {
        return CullVerdict::Paint;
    }
    if (clipped_children(node, *style, rect, frame)) {
        return CullVerdict::Subtree;
    }
    return bounded ? CullVerdict::Own : CullVerdict::Paint;
}

} // namespace tessera::ui::paint::detail
