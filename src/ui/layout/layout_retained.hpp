// SPDX-License-Identifier: MIT
//
// The retained Yoga tree (issue #735, slice D). `LayoutEngine::layout` used to build a `YGNode` per
// `LayoutInput`, apply ~40 computed-style lookups per node, lay out, and free the whole tree - 66%
// of a size-change frame. This keeps the tree across frames instead: one `YGNode` per node id, and
// only a node whose layout digest changed has its style rebuilt.
//
// Why a changed node's style is rebuilt from a pristine style rather than patched in place:
// `apply_style` writes only the longhands the computed style *has*, so a style edit that removes
// `width` would leave the old Yoga width behind. A pristine `Style` plus one `apply_style` is
// exactly what the from-scratch path produces, so the retained tree cannot drift from it. Only
// changed nodes pay that cost.
//
// The whole tree is rebuilt when the viewport changes, because `apply_style` bakes `vw`/`vh`/`rem`
// into points and the page adjustments depend on the viewport height.

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <yoga/Yoga.h>

namespace tessera::ui::text {
class TextMeasureCache;
}

namespace tessera::ui::layout::detail {

struct RetainedTreeState;

// What a node's Yoga style implies about how it is measured. A change here, in the style digest or
// in the parent's `display: block` is the complete set of reasons to rebuild a node's style.
enum class RetainedMeasure : std::uint8_t {
    None,
    Text,
    Grid,
};

// One retained node: the Yoga node plus everything its callbacks and the box walk read. The
// `LayoutInput` a callback used to point at is per-frame and dies with the call, so the tree owns
// its own copies of exactly the fields a callback reads. That ownership - not a lifetime
// assumption - is what makes the retained tree safe: the Yoga node's context is the `RetainedNode`
// itself, which lives exactly as long as the Yoga node does.
struct RetainedNode {
    YGNodeRef node = nullptr;
    std::uint64_t id = 0;
    RetainedTreeState* owner = nullptr;

    // The layout digest of the style this node's Yoga style was built from. `style::layout_digest`
    // covers exactly the longhands `apply_style`, the `order` permutation and `text_style_from`
    // read, so an equal digest means a rebuild would write the same Yoga style *and* the same
    // derived facts below. Zero means "never built" (the digest maps zero to one, so a real digest
    // is never zero).
    std::uint64_t style_digest = 0;
    // The parent's `display: block`, which decides `apply_block_child_style`.
    bool block_parent = false;

    // How this node is measured. A change is the only reason to touch the measure function, which
    // Yoga forbids on a node with children.
    RetainedMeasure measure = RetainedMeasure::None;

    // Style-derived facts the frame path reads, refreshed with the style so a steady frame touches
    // no computed-style map at all.
    bool grid = false;
    bool display_none = false;
    bool block = false;
    bool rtl = false;
    bool sizes_to_content = false;
    // CSS `order`, read once per style application instead of once per child per frame.
    int order = 0;

    // Owned callback storage: the text, its resolved text style, and the padding the text-fit fact
    // reads. `text_style` is a value, so no style entry has to outlive the frame.
    std::string text;
    text::TextStyle text_style;
    bool text_wrap = false;
    float pad_left = 0.0f;
    float pad_top = 0.0f;
    float pad_right = 0.0f;
    float pad_bottom = 0.0f;

    // The text-fit fact of the last box walk and what it was measured against. A steady frame
    // repeats it for every text node, and re-deriving it hashes the text and the style through the
    // measure cache each time. The fact is a function of this node's text, text style, padding and
    // box size plus the cache's contents, so it stays valid until one of those moves: the style or
    // text writes below clear `fit_valid`, the box size is compared, and `fit_generation` is the
    // cache's `generation()`. Mutable because the box walk reads the tree as const.
    mutable bool fit_valid = false;
    mutable TextFit fit_result = TextFit::Unknown;
    mutable float fit_width = 0.0f;
    mutable float fit_height = 0.0f;
    mutable std::uint64_t fit_generation = 0;

    // A grid container is a Yoga leaf whose measure callback runs `GridEngine` over the whole
    // subtree, so the tree owns that subtree's input and refreshes it only when its signature
    // changes. Nothing else points into it.
    std::unique_ptr<LayoutInput> grid_input;
    std::uint64_t grid_signature = 0;

    // Children in layout (CSS `order`) order, mirroring the Yoga child order exactly. `desired` is
    // this pass's target list, kept per node so a nested container cannot clobber its parent's and
    // a steady frame allocates nothing.
    std::vector<RetainedNode*> children;
    std::vector<RetainedNode*> desired;
    // The node each input child synced to last pass, in input order (`children` is in layout order,
    // which `order` permutes). A steady frame hands the same children in the same order, so the
    // i-th input child is checked against `source[i]` by id before the id-keyed map is asked: an
    // integer compare instead of a hash lookup per node. Every entry is a live node - it was in the
    // last pass's `desired`, which reconcile keeps - and the list is emptied wherever children are
    // destroyed wholesale (`set_measure`).
    std::vector<RetainedNode*> source;

    // The layout pass that last reached this node; anything older has left the input tree.
    std::uint64_t visited = 0;
};

struct RetainedTreeState {
    std::unordered_map<std::uint64_t, std::unique_ptr<RetainedNode>> nodes;
    // A node with the default style, used to reset a rebuilt node's Yoga style. Never inserted.
    YGNodeRef pristine = nullptr;
    RetainedNode* root = nullptr;
    LayoutViewport viewport;
    text::TextMeasureCache* cache = nullptr;
    // Scratch reused across the pass so a steady frame allocates nothing.
    std::uint64_t pass = 0;
    bool primed = false;
    // The pass cannot be completed incrementally: two input nodes shared an id, or a node moved to
    // a different parent. The caller discards the tree and runs one from-scratch pass rather than
    // producing boxes for the wrong node.
    bool restart = false;
    std::size_t style_applications = 0;
    std::size_t rebuilds = 0;
};

class RetainedTree {
public:
    RetainedTree();
    ~RetainedTree();

    RetainedTree(RetainedTree&&) noexcept;
    RetainedTree& operator=(RetainedTree&&) noexcept;
    RetainedTree(const RetainedTree&) = delete;
    RetainedTree& operator=(const RetainedTree&) = delete;

    [[nodiscard]] LayoutResult layout(const LayoutInput& root, const LayoutViewport& viewport,
                                      text::TextMeasureCache& cache);

    // Diagnostics for the differential test, not a frame-path cost.
    [[nodiscard]] std::size_t node_count() const;
    [[nodiscard]] std::size_t style_applications() const;
    [[nodiscard]] std::size_t rebuilds() const;

private:
    std::unique_ptr<RetainedTreeState> state_;
};

// Appends the boxes of `retained` and its subtree, in the same order the from-scratch walk emits
// them (layout_retained_collect.cpp).
void collect_retained_boxes(const RetainedNode& retained, float parent_x, float parent_y,
                            const RetainedTreeState& state, LayoutResult& result);

} // namespace tessera::ui::layout::detail
