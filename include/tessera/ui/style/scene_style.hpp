// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
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

private:
    struct Entry {
        const Node* node = nullptr;
        StyleNode view;
    };

    void build(const Node& node, StyleNode* parent);
    [[nodiscard]] StyleNode* mutable_view(const Node& node);

    std::deque<Entry> entries_;
    std::unordered_map<const Node*, std::size_t> index_;
    // Stable storage for the whitespace-split class-name tokens the views point at. A deque keeps
    // references valid while the tree is built, so a view never points at a reallocated buffer.
    std::deque<std::string> class_storage_;
};

} // namespace tessera::ui::style
