// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/paint/text_layout_cache.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::ui::paint {

// Resolves the computed style of one scene node. The paint pass never parses CSS; the caller
// owns the stylesheet and decides how a style is produced (a compiled stylesheet, an animation
// overlay, or a test stub).
class StyleProvider {
public:
    virtual ~StyleProvider() = default;

    [[nodiscard]] virtual style::ComputedStyle compute(const Node& node) = 0;
};

// The default provider over a compiled stylesheet and a SceneStyleTree captured from the same
// scene. Reconstruct the tree when children are added or removed (see SceneStyleTree).
class SceneStyleProvider final : public StyleProvider {
public:
    SceneStyleProvider(const style::CompiledStyleSheet& sheet, style::SceneStyleTree& tree);

    [[nodiscard]] style::ComputedStyle compute(const Node& node) override;

private:
    const style::CompiledStyleSheet* sheet_ = nullptr;
    style::SceneStyleTree* tree_ = nullptr;
};

struct PaintStats {
    std::size_t nodes = 0;           // visible nodes visited (style resolved)
    std::size_t backgrounds = 0;     // background quads emitted
    std::size_t borders = 0;         // border edge quads emitted
    std::size_t hidden_subtrees = 0; // invisible roots skipped (their subtrees are not visited)
    std::size_t clips = 0;           // overflow clips pushed (rounded or rectangular)
    std::size_t texts = 0;           // text runs emitted (non-empty PaintData)
    std::size_t vectors = 0;         // embedded-vector draws emitted (PaintKind::Vector)
    std::size_t images = 0;          // raster-image draws emitted (PaintKind::Image)
    std::size_t gradients = 0;       // linear-gradient fills emitted
    std::size_t shadows = 0;         // box-shadow fills emitted (one per blur layer)
    std::size_t transforms = 0;      // nodes painted in their own transformed frame
};

// Read-only, paint-only override of a node's visual state, used by fixed-step interpolation
// (ui/frame_interpolation.hpp). The pass applies it to its own copy of the computed style and to
// the transform it paints with. The scene graph is never touched: Node::bounds()/transform(),
// Node::world_bounds()/hit_test(), dirty flags, damage, and layout keep the tick geometry, so a
// render between ticks can never move a box or make hit testing disagree with layout.
class PaintView {
public:
    virtual ~PaintView() = default;

    // Local transform (around the node's own box origin, the shape Node::set_transform takes) to
    // paint with instead of Node::transform(). Nullopt paints the node's authoritative transform.
    // Inside an overridden subtree, painted descendants compose from this matrix, so a translated
    // node moves its children rigidly.
    [[nodiscard]] virtual std::optional<rendering::rhi::Mat3>
    local_transform(NodeId node) const = 0;

    // Overrides the paint-visible properties of `style` that this view knows for `node`, in place.
    // The paint pass reads colors and opacity from the result; nothing is written back to the
    // scene.
    virtual void overlay_style(NodeId node, style::ComputedStyle& style) const = 0;
};

// Optional per-paint context. The cache is caller-owned (paint frame/sink) and deliberately not a
// global singleton: pass the same instance across frames to reuse wrapped line boxes. Null means
// every paint recomputes text layout (the uncached path). `text_metrics_source` selects the wrap
// measurement source: null = the deterministic stub (existing goldens/tests byte-identical), or a
// caller-owned `PaintTextMetricsSource` (for example `FontPaintTextMetrics` for real FreeType
// metrics + HarfBuzz advances). Its id is part of the cache key, so sources never mix.
struct PaintContext {
    TextLayoutCache* text_layout_cache = nullptr;
    const PaintTextMetricsSource* text_metrics_source = nullptr;
    // Device scale used for caret width (1 px at scale 1, 2 px at scale >= 2) and future
    // decoration metrics. Defaults to 1 so tests/goldens are unchanged.
    float device_scale = 1.0f;
    // Optional paint-only visual override (fixed-step interpolation). Null paints the scene's
    // authoritative visual state. The view never mutates the scene.
    const PaintView* view = nullptr;
};

// Paints the box visuals of a scene subtree: background-color and the four border edges, in
// pre-order (parent before children, children in paint order). Does not mutate the scene.
//
// Coordinates: bounds() are parent-relative and Node::world_bounds() composes their origins,
// transforms, and any ancestor scroll offset into absolute coordinates, so the pass paints that
// rect. Rotation/scale are approximated by the world_bounds() AABB. Scroll containers (see
// Node::scrollable_x/scrollable_y) clip their children to their padding box - the border box inset
// by the painted border widths - through `PaintSink::push_clip`; their children are offset by
// -scroll by the world transform.
//
// Limits (deliberate, one slice): no gradients, shadows, or layers, no percentage/em lengths, and
// no per-node compositing. Opacity multiplies the alpha of every color the node emits; it does not
// isolate a group. Content runs come from PaintData: text (section 10 of the paint-pass note) or a
// single embedded vector asset (section 11) or raster image (section 12), one active kind per node.
//
// Paint order per node: background, then the top, right, bottom, and left border edges. A uniform
// non-zero corner radius paints the background as one rounded rect; borders stay plain edge rects,
// so their corners are square over the rounded background. Border styles other than none/hidden are
// painted solid. Unparseable color values and zero-alpha colors are skipped.
PaintStats paint_scene(const Node& root, StyleProvider& styles, PaintSink& sink,
                       const PaintContext& context = {});

} // namespace tessera::ui::paint
