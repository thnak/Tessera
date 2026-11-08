// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::ui {

namespace rhi = tessera::rendering::rhi;

using NodeId = std::uint64_t;

class Node {
public:
    explicit Node(NodeId id = 0);
    virtual ~Node() = default;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    [[nodiscard]] NodeId id() const {
        return id_;
    }
    [[nodiscard]] Node* parent() const {
        return parent_;
    }

    [[nodiscard]] const std::vector<std::unique_ptr<Node>>& children() const {
        return children_;
    }
    Node& add_child(std::unique_ptr<Node> child);
    // Detaches and returns the owned child so callers can destroy or reattach a subtree. Returns
    // nullptr when `child` is not a direct child of this node.
    [[nodiscard]] std::unique_ptr<Node> remove_child(const Node& child);

    void set_bounds(const rhi::Rect& bounds);
    [[nodiscard]] const rhi::Rect& bounds() const {
        return bounds_;
    }

    void set_transform(const rhi::Mat3& transform);
    [[nodiscard]] const rhi::Mat3& transform() const {
        return transform_;
    }

    void set_visible(bool visible);
    [[nodiscard]] bool visible() const {
        return visible_;
    }

    void set_focusable(bool focusable);
    [[nodiscard]] bool focusable() const {
        return focusable_;
    }

    // Sequential focus order key. The ordering contract lives on FocusManager::focus_next.
    void set_tab_index(int tab_index);
    [[nodiscard]] int tab_index() const {
        return tab_index_;
    }

    // Interaction state seen by selector matching. Focus changes update the `Focus` bit and mark
    // the style domain dirty so the resolver re-evaluates `:focus`/`:not(:focus)`.
    void set_pseudo_state(style::PseudoState state);
    void set_pseudo_state(style::PseudoState state, bool enabled);
    [[nodiscard]] style::PseudoState pseudo_state() const {
        return pseudo_state_;
    }

    void mark_dirty(Dirty flags);
    void clear_dirty(Dirty flags);
    [[nodiscard]] Dirty dirty() const {
        return dirty_;
    }
    [[nodiscard]] Dirty subtree_dirty() const {
        return subtree_dirty_;
    }

    [[nodiscard]] rhi::Mat3 world_transform() const;
    [[nodiscard]] rhi::Rect world_bounds() const;

    [[nodiscard]] bool hit_test(float x, float y) const;
    [[nodiscard]] Node* hit_test_deep(float x, float y);

    virtual void on_event(Event& event);

private:
    void refresh_subtree_dirty();

    NodeId id_ = 0;
    Node* parent_ = nullptr;
    std::vector<std::unique_ptr<Node>> children_;
    rhi::Rect bounds_;
    rhi::Mat3 transform_;
    bool visible_ = true;
    bool focusable_ = false;
    int tab_index_ = 0;
    style::PseudoState pseudo_state_ = style::PseudoState::None;
    Dirty dirty_ = Dirty::None;
    Dirty subtree_dirty_ = Dirty::None;
};

bool dispatch_event(Node& root, Event& event);
bool dispatch_event_to(Node& target, Event& event);

} // namespace tessera::ui
