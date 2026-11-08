// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace tessera::ui {
namespace {

bool same_rect(const rhi::Rect& lhs, const rhi::Rect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

bool same_transform(const rhi::Mat3& lhs, const rhi::Mat3& rhs) {
    for (int i = 0; i < 9; ++i) {
        if (lhs.m[i] != rhs.m[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

Node::Node(NodeId id)
    : id_(id) {}

Node& Node::add_child(std::unique_ptr<Node> child) {
    child->mark_dirty(Dirty::Style | Dirty::Layout | Dirty::Paint | Dirty::Render);
    child->parent_ = this;
    children_.push_back(std::move(child));
    mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
    refresh_subtree_dirty();
    return *children_.back();
}

void Node::set_bounds(const rhi::Rect& bounds) {
    if (same_rect(bounds_, bounds)) {
        return;
    }
    bounds_ = bounds;
    mark_dirty(Dirty::Paint | Dirty::Render);
}

void Node::set_transform(const rhi::Mat3& transform) {
    if (same_transform(transform_, transform)) {
        return;
    }
    transform_ = transform;
    mark_dirty(Dirty::Paint | Dirty::Render);
}

void Node::set_visible(bool visible) {
    if (visible_ == visible) {
        return;
    }
    visible_ = visible;
    mark_dirty(Dirty::Layout | Dirty::Paint | Dirty::Render);
}

void Node::mark_dirty(Dirty flags) {
    if (flags == Dirty::None) {
        return;
    }

    dirty_ |= flags;
    subtree_dirty_ |= flags;
    for (Node* ancestor = parent_; ancestor != nullptr; ancestor = ancestor->parent_) {
        if ((ancestor->subtree_dirty_ & flags) == flags) {
            break;
        }
        ancestor->subtree_dirty_ |= flags;
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

rhi::Mat3 Node::world_transform() const {
    if (parent_ != nullptr) {
        return parent_->world_transform() * transform_;
    }
    return transform_;
}

rhi::Rect Node::world_bounds() const {
    const rhi::Mat3 world = world_transform();
    const float xs[4] = {bounds_.x, bounds_.right(), bounds_.right(), bounds_.x};
    const float ys[4] = {bounds_.y, bounds_.y, bounds_.bottom(), bounds_.bottom()};

    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();

    for (int i = 0; i < 4; ++i) {
        float x = 0;
        float y = 0;
        world.transform_point(xs[i], ys[i], x, y);
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }

    return rhi::Rect{min_x, min_y, max_x - min_x, max_y - min_y};
}

bool Node::hit_test(float x, float y) const {
    if (!visible_) {
        return false;
    }

    rhi::Mat3 inverse;
    if (!world_transform().try_inverse(inverse)) {
        return false;
    }

    float local_x = 0;
    float local_y = 0;
    inverse.transform_point(x, y, local_x, local_y);
    return bounds_.contains(local_x, local_y);
}

Node* Node::hit_test_deep(float x, float y) {
    if (!visible_) {
        return nullptr;
    }

    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
        if (Node* hit = (*it)->hit_test_deep(x, y)) {
            return hit;
        }
    }

    return hit_test(x, y) ? this : nullptr;
}

void Node::on_event(Event&) {}

} // namespace tessera::ui
