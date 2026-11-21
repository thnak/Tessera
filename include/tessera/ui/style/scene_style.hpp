// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace tessera::ui::style {

// Materializes selector-matching views for a scene subtree so the stylesheet engine can evaluate
// scene nodes without owning them. The view exposes tree structure, pseudo-state, and the element
// metadata the scene already stores (docs/design/scene-graph.md section 5): `tag` from
// `Node::element_kind()`, `id` from `Node::element_id()`, `classes` from the whitespace-separated
// `Node::class_name()`, and `has_text` from a `PaintKind::Text` payload. Attribute selectors see
// no attributes yet: `aria-*` metadata is owned by the accessibility lane and is not a selector
// input. Reconstruct the tree when children are added or removed.
//
// The scene stays the source of truth for interaction state: `view`/`compute` refresh the cached
// view from `Node::pseudo_state()`, so a tree captured before a focus change still resolves
// `:focus`/`:not(:focus)` against the new state. Reconstruct the tree when children are added or
// removed.
//
// `@container` rules resolve against ancestors whose computed `container-type` is `size` or
// `inline-size`. The host supplies each container's content-box size (typically from the layout
// read model) through `ContainerSizeFn`; a container with no size yet (before the first layout)
// leaves its rules unapplied, and the settle round re-resolves once it has one.
class SceneStyleTree {
public:
    // Writes the content-box width and height of `container`; returns false when it has none.
    using ContainerSizeFn = bool (*)(void* context, const Node& container, ContainerSize& out);

    explicit SceneStyleTree(const Node& root);

    // Returns the selector view for `node`, refreshing its pseudo-state from the scene node.
    // Returns nullptr when `node` is not part of the captured tree.
    [[nodiscard]] const StyleNode* view(const Node& node);

    // Computes the style for `node` from `sheet`, cascading through its captured ancestors. Nodes
    // outside the captured tree resolve to the registry's initial style.
    [[nodiscard]] ComputedStyle compute(const CompiledStyleSheet& sheet, const Node& node,
                                        ContainerSizeFn container_size = nullptr,
                                        void* container_context = nullptr);

    // A per-generation memo (issue #711). Each node's computed style is computed once from its
    // parent's memoised style (instead of recomputing the whole ancestor chain per node, which made
    // a node's cost proportional to its depth), and `resolve` hands out references into it.
    // `begin_memo` starts a generation; the entries stay valid after `end_memo` so the paint pass
    // reads them instead of re-cascading every culled node. A generation is one selector tree:
    // every style input (class, id, attribute, pseudo-state, inline style, theme, media) marks
    // `Dirty::Style`, which makes the host rebuild the tree and restyle, so a stale entry never
    // outlives the edit that changed it. Container queries are not memoised (`compute` with a
    // container source keeps the chain walk).
    // `keep_entries` continues the previous generation: only the subtrees `restyle_subtree` erased
    // are recomputed.
    void begin_memo(bool keep_entries = false);
    void end_memo();
    [[nodiscard]] bool memo_active() const {
        return memo_active_;
    }
    // Same value as `compute(sheet, node)`, without a copy; the reference lives until the next
    // `begin_memo` or the tree's destruction. Requires an active memo.
    [[nodiscard]] const ComputedStyle& resolve(const CompiledStyleSheet& sheet, const Node& node);

    // Re-reads the selector views of `node` and its descendants from the scene and drops their
    // memoised styles, so the next `resolve` recomputes exactly that subtree (a node's computed
    // style depends only on itself and its ancestors; see `CompiledStyleSheet::subtree_local`).
    // Valid only while the scene's shape is unchanged (`built_for_structure_epoch`).
    void restyle_subtree(const Node& node);
    [[nodiscard]] std::uint64_t built_for_structure_epoch() const {
        return structure_epoch_;
    }

private:
    struct Entry {
        const Node* node = nullptr;
        StyleNode view;
        // Backing storage of the class names `view.classes` points at.
        std::vector<std::string> class_tokens;
    };

    void build(const Node& node, StyleNode* parent);
    void fill_view(Entry& entry, const Node& node, StyleNode* parent);
    void refresh(const Node& node, StyleNode* parent);
    [[nodiscard]] StyleNode* mutable_view(const Node& node);

    bool memo_active_ = false;
    // Computed styles (inline declarations included) by node. Node based, so references stay valid
    // while the memo grows.
    std::unordered_map<const Node*, ComputedStyle> memo_;
    std::deque<Entry> entries_;
    std::unordered_map<const Node*, std::size_t> index_;
    std::uint64_t structure_epoch_ = 0;
};

} // namespace tessera::ui::style
