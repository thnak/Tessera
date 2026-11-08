// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/scene/event.hpp"

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
    Dirty dirty_ = Dirty::None;
    Dirty subtree_dirty_ = Dirty::None;
};

bool dispatch_event(Node& root, Event& event);
bool dispatch_event_to(Node& target, Event& event);

} // namespace tessera::ui
