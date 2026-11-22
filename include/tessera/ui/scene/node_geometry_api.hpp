// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/scene/node_types.hpp"

// Box geometry and paint-extent surface of `ui::Node` (docs/architecture/god-type-audit-v1.md
// section 3.1): the border box, the inline-span override, the local transform, the stacking index,
// the paint outsets, the glass paint inputs, and everything derived from them (`world_*`, the
// cached subtree paint box). Hit testing stays on `Node` itself because it is the mouse-move hot
// path and reads the scroll insets as well.
//
// A data-free base of `Node`: the fields stay inline in `Node` (see `NodeInteractionApi` for why),
// and the bodies reach them through `node()`. Bodies are in `node_geometry.cpp`, the one-line reads
// in `node_geometry_api_inl.hpp`.
namespace tessera::ui {

namespace rhi = tessera::rendering::rhi;

class Node;

class NodeGeometryApi {
public:
    void set_bounds(const rhi::Rect& bounds);
    [[nodiscard]] const rhi::Rect& bounds() const;
    // Inline-span run rectangle, in the parent's border-box coordinates (issue #229). A span is not
    // a layout box: the paint pass derives this rect from the parent's line boxes and stores it so
    // the generic hit test can resolve a point to the span child. It never marks a dirty domain;
    // the parent's layout/paint dirty flags already drive the frame (a span has no independent
    // size).
    //
    // `const` because this is paint-derived data written through a const scene (issue #733 item 4):
    // the pass is handed the tree as `const` and must not have to `const_cast` a node to record
    // what it derived. The same rule as `subtree_paint_bounds_` -- a field paint fills lazily is
    // `mutable`, so "paint does not change the scene" stays true for every field a caller can read
    // as scene state.
    void set_span_bounds(const rhi::Rect& bounds) const;

    void set_transform(const rhi::Mat3& transform);
    [[nodiscard]] const rhi::Mat3& transform() const;
    // False while the transform is the identity: a hot-line flag, so a pass that only needs to know
    // whether to apply it does not read the 36-byte matrix.
    [[nodiscard]] bool has_transform() const;

    // Stacking order among siblings (CSS `z-index`, `auto` = 0): lower values paint first and hit
    // last; equal values keep child order. Visual only, so it dirties paint/render, not layout.
    void set_z_index(int z_index);
    [[nodiscard]] int z_index() const;
    // True when `z_index()` is non-zero. Sibling walks test this first: when no child has one, the
    // stacking order is plain child order and the values are never read.
    [[nodiscard]] bool has_z_index() const;

    // How far this node paints beyond its border box (see `PaintOutsets`).
    void set_paint_outsets(const PaintOutsets& outsets);
    [[nodiscard]] const PaintOutsets& paint_outsets() const;
    [[nodiscard]] rhi::Rect subtree_paint_bounds() const; // cached; see node_geometry.cpp
    void invalidate_subtree_paint_bounds() const;

    // Glass (CSS `backdrop-filter`, docs/specs/glass-v1.md sections 2 and 7). The blur sigma in CSS
    // px is what paint::sync_scene_style resolved from the computed value, already clamped to the
    // configured `glassMaxBlurRadius`; 0 means the node has no non-`none` backdrop-filter. Damage
    // uses it to grow the node's paint dependency region by 3 sigma. `glass_subtree` is the derived
    // "this subtree contains a glass node" flag, so a frame whose tree has no glass node never
    // walks the scene looking for one. Both are paint-only: hit testing and layout ignore them.
    void set_glass_blur_sigma(float sigma);
    [[nodiscard]] float glass_blur_sigma() const;
    void set_glass_subtree(bool has_glass);
    [[nodiscard]] bool glass_subtree() const;

    // Layout's text-fit fact (issue #490): true when layout proved this node's text payload stays
    // inside the content box paint uses. `apply_layout` rewrites it every pass, so it is false
    // until one proved it; paint re-checks the payload before trusting it.
    void set_text_fits_box(bool fits);
    [[nodiscard]] bool text_fits_box() const;

    [[nodiscard]] rhi::Mat3 world_transform() const;
    [[nodiscard]] rhi::Rect world_bounds() const;

protected:
    NodeGeometryApi() = default;
    ~NodeGeometryApi() = default;

private:
    [[nodiscard]] Node& node() noexcept;
    [[nodiscard]] const Node& node() const noexcept;
};

} // namespace tessera::ui
