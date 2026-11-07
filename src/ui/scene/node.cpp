// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace tessera::ui {
namespace {

constexpr float kEpsilon = 1.0e-6f;

} // namespace

Node::Node(NodeId id)
    : id_(id) {}

Node& Node::add_child(std::unique_ptr<Node> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
    return *children_.back();
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
