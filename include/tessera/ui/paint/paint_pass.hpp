// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cstddef>

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
};

// Paints the box visuals of a scene subtree: background-color and the four border edges, in
// pre-order (parent before children, children in paint order). Does not mutate the scene.
//
// Coordinates: a node's bounds() are parent-relative, so the pass adds the bounds origin of every
// ancestor to world_bounds(). That is exact for identity and translation-only transforms;
// rotation/scale are approximated by the world_bounds() AABB plus those origins. Per-node
// transforms are not composed further, and there is no clip/overflow.
//
// Limits (deliberate, one slice): no text, no clipping, no damage/redraw plan, no gradients,
// shadows, or layers, no percentage/em lengths, and no per-node compositing. Opacity multiplies
// the alpha of every color the node emits; it does not isolate a group.
//
// Paint order per node: background, then the top, right, bottom, and left border edges. A uniform
// non-zero corner radius paints the background as one rounded rect; borders stay plain edge rects,
// so their corners are square over the rounded background. Border styles other than none/hidden are
// painted solid. Unparseable color values and zero-alpha colors are skipped.
PaintStats paint_scene(const Node& root, StyleProvider& styles, PaintSink& sink);

} // namespace tessera::ui::paint
