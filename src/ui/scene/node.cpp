// SPDX-License-Identifier: MIT

// Tree structure and dirty-domain bookkeeping of `ui::Node`, plus the empty `on_event` default
// (docs/architecture/god-type-audit-v1.md section 3.1, slice 1).
//
// The header stayed unchanged in this slice: geometry moved to node_geometry.cpp, the scroll
// runtime to node_scroll.cpp, and the interaction policy to node_interaction.cpp, so a change to
// one of those features no longer edits the TU every other feature includes.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/structure_epoch.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace tessera::ui {

namespace {

std::atomic<std::uint64_t> g_structure_epoch{0};

} // namespace

std::uint64_t scene_structure_epoch() noexcept {
    return g_structure_epoch.load(std::memory_order_relaxed);
}

// The first-line layout of `Node` (see the member block in node.hpp) as compile-time facts: every
// field the whole-scene passes read at each node ends inside the first 64 bytes, and the first
// on-demand field starts exactly at the second line. A new member placed on line 0 by mistake, or
// a reorder that pushes a hot field across the boundary, fails here instead of silently costing a
// cache miss per visited node. 64-bit only: the offsets are a layout fact of 8-byte pointers and a
// 24-byte vector, and the size assert in node.hpp already pins the same lanes.
#if INTPTR_MAX == INT64_MAX
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
struct NodeLayoutCheck {
    // **The first-line claim is about the layout that ships.** It holds only when `children_` is a
    // 24-byte vector; MSVC's Debug `std::vector` is 32 bytes (`_ITERATOR_DEBUG_LEVEL != 0` carries
    // an allocator pointer), which pushes the byte-sized role words past offset 64 and moves
    // `extent_` off the line boundary (issue #896).
    //
    // Asserting the offsets in that configuration would be asserting something false: the objects
    // genuinely straddle two lines. What is worth pinning there is the *separation* - no extra
    // padding appears, and every field keeps its relative order - so this stays a layout guard in
    // both configurations instead of being switched off in one.
    //
    // The condition is the vector's width rather than a configuration name, because that is the
    // thing that actually moved the offsets; a future stdlib change gets the same two cases for
    // free.
    static constexpr bool kChildrenIsCompact = sizeof(Node::children_) == 24;

#define TESSERA_NODE_ON_FIRST_LINE(member)                                                         \
    static_assert(!kChildrenIsCompact ||                                                           \
                      offsetof(Node, member) + sizeof(Node::member) <= detail::kCacheLine,         \
                  #member " must stay on Node's first cache line")
    TESSERA_NODE_ON_FIRST_LINE(parent_);
    TESSERA_NODE_ON_FIRST_LINE(children_);
    TESSERA_NODE_ON_FIRST_LINE(bounds_);
    TESSERA_NODE_ON_FIRST_LINE(hints_);
    TESSERA_NODE_ON_FIRST_LINE(dirty_);
    TESSERA_NODE_ON_FIRST_LINE(subtree_dirty_);
    TESSERA_NODE_ON_FIRST_LINE(input_);
    TESSERA_NODE_ON_FIRST_LINE(scroll_style_);
#undef TESSERA_NODE_ON_FIRST_LINE
    static_assert(!kChildrenIsCompact || offsetof(Node, extent_) == detail::kCacheLine,
                  "the first on-demand field starts exactly on the second line");
    // The claim that survives both configurations: the hot fields precede the on-demand ones, and
    // no field was reordered. A reorder is a performance change that must not land silently,
    // whatever the stdlib's vector width is.
    static_assert(offsetof(Node, children_) < offsetof(Node, extent_),
                  "the tree links precede the on-demand members in both configurations");
    static_assert(offsetof(Node, extent_) < offsetof(Node, metadata_),
                  "the on-demand members keep their order");
    static_assert(offsetof(Node, parent_) == sizeof(void*),
                  "`parent_` starts right after the vptr, with no padding before it");
    static_assert(offsetof(Node, children_) == offsetof(Node, parent_) + sizeof(void*),
                  "`children_` follows `parent_` with no padding between them");
};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

Node::Node(NodeId id)
    : id_(id) {}

Node::~Node() = default;

void* Node::operator new(std::size_t size) {
    return ::operator new(size, std::align_val_t{detail::kCacheLine});
}

void Node::operator delete(void* pointer, std::size_t size) noexcept {
    ::operator delete(pointer, size, std::align_val_t{detail::kCacheLine});
}

Node& Node::add_child(std::unique_ptr<Node> child) {
    g_structure_epoch.fetch_add(1, std::memory_order_relaxed);
    child->mark_dirty(Dirty::Style | Dirty::Layout | Dirty::Paint | Dirty::Render);
    child->parent_ = this;
    children_.push_back(std::move(child));
    mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
    refresh_subtree_dirty();
    return *children_.back();
}

Node& Node::insert_child(std::size_t index, std::unique_ptr<Node> child) {
    g_structure_epoch.fetch_add(1, std::memory_order_relaxed);
    child->mark_dirty(Dirty::Style | Dirty::Layout | Dirty::Paint | Dirty::Render);
    child->parent_ = this;
    const std::size_t at = std::min(index, children_.size());
    const auto inserted =
        children_.insert(children_.begin() + static_cast<std::ptrdiff_t>(at), std::move(child));
    mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
    refresh_subtree_dirty();
    return **inserted;
}

std::unique_ptr<Node> Node::remove_child(const Node& child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() != &child) {
            continue;
        }
        g_structure_epoch.fetch_add(1, std::memory_order_relaxed);
        std::unique_ptr<Node> removed = std::move(*it);
        children_.erase(it);
        removed->parent_ = nullptr;
        mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
        refresh_subtree_dirty();
        return removed;
    }
    return nullptr;
}

void Node::mark_dirty(Dirty flags) {
    if (flags == Dirty::Clean) {
        return;
    }

    dirty_ |= flags;
    subtree_dirty_ |= flags;
    // The cached subtree paint box (issue #735, slice C) is invalidated here, in the one walk every
    // paint-visible mutation already takes, rather than at each call site: a mutation that can move
    // a painted pixel marks Paint, and this reaches the node and every ancestor. That is what makes
    // the invalidation complete - see subtree_paint_bounds() for the argument. The walk runs to the
    // root even when the dirty propagation below can stop early, because a node whose
    // `subtree_dirty_` already carries Paint may have got it from `refresh_subtree_dirty` (a
    // child's clear), which does not invalidate anything.
    if ((flags & Dirty::Paint) != Dirty::Clean) {
        for (Node* node = this; node != nullptr; node = node->parent_) {
            node->extent_.invalidate_cache();
        }
    }
    for (Node* ancestor = parent_; ancestor != nullptr; ancestor = ancestor->parent_) {
        if ((ancestor->subtree_dirty_ & flags) == flags) {
            break;
        }
        ancestor->subtree_dirty_ |= flags;
    }
}

void Node::mark_subtree_dirty(Dirty flags) {
    if (flags == Dirty::Clean) {
        return;
    }
    mark_dirty(flags);
    for (const auto& child : children_) {
        child->mark_subtree_dirty(flags);
    }
}

void Node::clear_dirty(Dirty flags) {
    const Dirty remaining = dirty_ & ~flags;
    if (remaining == dirty_) {
        return;
    }
    dirty_ = remaining;
    refresh_subtree_dirty();
}

void Node::clear_subtree_dirty(Dirty flags) {
    if (flags == Dirty::Clean || !has_any(subtree_dirty_, flags)) {
        return;
    }
    clear_subtree_dirty_below(flags);
    // Only the ancestors of the cleared root can still be stale; they re-derive from their
    // children.
    if (parent_ != nullptr) {
        parent_->refresh_subtree_dirty();
    }
}

void Node::clear_subtree_dirty_below(Dirty flags) {
    // Post-order: each node recomputes its summary exactly once, from children that are already
    // final. Clearing node by node with `clear_dirty` made every child's change re-scan the whole
    // child list of its parent, which is quadratic in the width of a wide container.
    Dirty next = dirty_ & ~flags;
    for (const auto& child : children_) {
        if (has_any(child->subtree_dirty_, flags)) {
            child->clear_subtree_dirty_below(flags);
        }
        next |= child->subtree_dirty_;
    }
    dirty_ = dirty_ & ~flags;
    subtree_dirty_ = next;
}

void Node::refresh_subtree_dirty() {
    Dirty next = dirty_;
    for (const auto& child : children_) {
        next |= child->subtree_dirty();
    }
    if (next == subtree_dirty_) {
        return;
    }
    subtree_dirty_ = next;
    if (parent_ != nullptr) {
        parent_->refresh_subtree_dirty();
    }
}

void Node::on_event(Event&) {}

} // namespace tessera::ui
