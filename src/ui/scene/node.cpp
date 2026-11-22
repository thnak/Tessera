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
#include <utility>

namespace tessera::ui {

namespace {

std::atomic<std::uint64_t> g_structure_epoch{0};

} // namespace

std::uint64_t scene_structure_epoch() noexcept {
    return g_structure_epoch.load(std::memory_order_relaxed);
}

Node::Node(NodeId id)
    : id_(id) {}

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
            node->subtree_paint_bounds_ = rhi::Rect{0.0f, 0.0f, -1.0f, 0.0f};
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
