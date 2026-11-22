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
//     a node that paints text is only culled on the box test when layout proved the payload fits
//     (`Node::text_fits_box`, issue #490) and the payload is still the plain text layout measured -
//     see `own_emission_bounded`; otherwise it is only ever culled as a `Subtree`, and only when
//     the pass clips it;
//   - children are only bounded by an actual clip push (overflow or scroll), never by the box: a
//     non-clipping parent's children may overflow anywhere, so its own emission is skipped but its
//     children are still walked (`Own`).
// The fit fact is a layout output, so it is exactly as fresh as the last layout pass:
// `apply_layout` rewrites it (including `Unknown`) on every pass and a node that has never been
// laid out reads false, so a hint can only ever be *more* conservative than the truth, never stale
// in the unsafe direction. A node painted in its own transform frame never reaches this decision
// (the caller refuses it).

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
#include <cstdint>
#include <optional>

namespace tessera::ui::paint::detail {

enum class CullVerdict : std::uint8_t {
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

// True when the payload paint would emit for a text node is exactly the plain text run layout
// measured, so `Node::text_fits_box` actually describes it. Layout's measure never sees
// `PaintData`, so anything the paint pass adds on top of that plain text disqualifies the hint and
// the node falls back to the pre-#490 conservative rule. Inline spans paint one run per span with
// that span's own resolved style, which is not the plain-text measure. Any caret/selection/IME
// decoration paints the laid-out line verbatim (no ellipsis truncation) and draws its caret rect at
// the line end, where its own 1-2 device px width reaches past the measured line; an active
// composition also paints the uncommitted preedit run, which is not part of the measured text at
// all. Disqualifying every decoration rather than only `composition_active` is deliberate: the
// caret is the tighter of the two cases and the cost is a field that is edited rather than scrolled
// past. `max_lines` truncation is measured by layout with the full text, so the truncation itself
// is not covered by the measure. A payload `font_size` override re-measures the run with metrics
// layout never saw (advances scale with the size), so only the absence of an override is provable
// without resolving the style; a `font_weight` override is refused for the same reason, because the
// hint is about the computed style. `text-overflow: ellipsis` is deliberately not listed: it only
// ever shortens a line, so it cannot make paint exceed the content box. `clip` paints the laid-out
// line verbatim, which is exactly the geometry the measure bounds.
inline bool text_payload_is_plain(const PaintData& data) {
    return data.spans.empty() && !data.decoration.has_value() && data.max_lines == 0 &&
           data.text_style.font_size <= 0.0f && data.text_style.font_weight <= 0;
}

// True when everything the node paints for itself is inside the reach: box visuals (background,
// border, gradient, backdrop, box shadow) are inside the border box or its synced shadow outsets,
// and the vector and image payloads are placed at the content box. Text is the one payload that can
// leave the box: `paint_text_lines` advances the line origin past the content box for every wrapped
// line it emits and does not clip, so a box test alone cannot bound an unclipped text run. Layout
// answers that question for a plain payload (`Node::text_fits_box`, issue #490); when it has not,
// or the payload is richer than what layout measured, the node stays unbounded here.
//
// `rect` is the rect paint passes to `paint_text_lines`. The fit fact was computed from the
// *layout* box, so it is only trusted while paint's content box is at least as large as the
// measured one: a transform frame or a PaintView interpolation can make paint's rect the
// transformed AABB of the box (a scaled-down ancestor shrinks it), and the text is still laid out
// at the unscaled font size in that rect. `rect.width >= bounds.width` is the provable form of
// "paint has at least the room layout measured"; the tolerance only absorbs the float rounding of
// the box origin when `world_bounds()` maps a translation (below a device pixel, and below the
// one-pixel AA slack `reaches_cull` already carries). A real scale changes the size by far more and
// falls back to unbounded.
inline bool own_emission_bounded(const Node& node, const rhi::Rect& rect) {
    const auto& data = node.paint_data();
    if (!data.has_value() || data->kind != PaintKind::Text) {
        return true;
    }
    const bool paints_content =
        !data->text.empty() || !data->spans.empty() ||
        (data->decoration.has_value() && data->decoration->composition_active);
    if (!paints_content) {
        return true;
    }
    if (!node.text_fits_box() || !text_payload_is_plain(*data)) {
        return false;
    }
    constexpr float kOriginRounding = 1.0e-3f;
    return rect.width + kOriginRounding >= node.bounds().width &&
           rect.height + kOriginRounding >= node.bounds().height;
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

// Subtree cull (issue #735, slice C): true when the node's cached subtree paint box, mapped into
// device space by the matrix the node paints with, cannot touch `cull`. This rejects a whole
// subtree at its root in O(1) instead of visiting every node in it, which is what the per-node
// `reaches_cull` above cannot do: that test only ever skips one node's own emission.
//
// The box is a superset of every pixel the subtree can paint (see Node::subtree_paint_bounds), so a
// miss here is a miss for the whole subtree. A non-finite box means "cannot be bounded" and is
// never culled. `painted_world` must be the node's authoritative world matrix: an interpolated
// frame moves painted geometry without touching the scene, so the cached box would be stale, and
// the caller refuses this test for those nodes.
inline bool subtree_misses_cull(const Node& node, const rhi::Mat3& painted_world,
                                const rhi::Rect& cull) {
    const rhi::Rect box = node.subtree_paint_bounds();
    if (!std::isfinite(box.x) || !std::isfinite(box.y) || !std::isfinite(box.width) ||
        !std::isfinite(box.height)) {
        return false; // unbounded: never cull
    }
    const rhi::Rect reach = transformed_aabb(painted_world, box);
    if (!std::isfinite(reach.x) || !std::isfinite(reach.y) || !std::isfinite(reach.width) ||
        !std::isfinite(reach.height)) {
        return false;
    }
    return reach.x >= cull.right() || reach.right() <= cull.x || reach.y >= cull.bottom() ||
           reach.bottom() <= cull.y;
}

// Whether the paint walk may take the subtree skip for this node. `interpolated` and `open_frame`
// both mean the node paints through a matrix the scene does not know, and a dirty node must still
// be visited so its paint-cache record is refreshed rather than going stale.
inline bool may_skip_subtree(const Node& node, const rhi::Mat3& painted_world,
                             const rhi::Rect& cull, bool interpolated, bool open_frame,
                             bool records_dirty) {
    return !interpolated && !open_frame && !records_dirty &&
           subtree_misses_cull(node, painted_world, cull);
}

// The cull verdict for a node whose reach already missed the rect and that does not paint in its
// own transform frame. `style` is the node's computed style; the caller must provide it when the
// node has children (both the clip decision and the opacity its children inherit need it) and may
// leave it null for a leaf, which is the point: a leaf outside the rect never resolves a style.
inline CullVerdict cull_verdict(const Node& node, const rhi::Rect& rect, const Frame& frame,
                                const style::ComputedStyle* style) {
    const bool bounded = own_emission_bounded(node, rect);
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
