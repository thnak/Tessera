// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/focus.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace tessera::ui {
namespace {

void dispatch_focus_event(Node& target, EventType type) {
    Event event;
    event.type = type;
    dispatch_event_to(target, event);
}

void collect_focusable(Node& node, std::vector<Node*>& order) {
    if (!node.visible()) {
        return;
    }
    if (node.focusable()) {
        order.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_focusable(*child, order);
    }
}

} // namespace

void FocusManager::set_root(Node& root) {
    root_ = &root;
}

bool FocusManager::is_attached(const Node& node) const {
    if (root_ != nullptr) {
        for (const Node* ancestor = &node; ancestor != nullptr; ancestor = ancestor->parent()) {
            if (ancestor == root_) {
                return true;
            }
        }
        return false;
    }
    return node.parent() != nullptr;
}

bool FocusManager::is_valid(const Node& node) const {
    if (!node.focusable() || !node.visible() || !is_attached(node)) {
        return false;
    }
    for (const Node* ancestor = node.parent(); ancestor != nullptr; ancestor = ancestor->parent()) {
        if (!ancestor->visible()) {
            return false;
        }
    }
    return true;
}

bool FocusManager::request_focus(Node* node) {
    if (node == nullptr || !is_valid(*node)) {
        return false;
    }
    if (focused_ == node) {
        return true;
    }

    Node* previous = focused_;
    focused_ = node;
    if (previous != nullptr) {
        dispatch_focus_event(*previous, EventType::Blur);
        // A Blur handler may have moved focus again; do not overwrite that decision.
        if (focused_ != node) {
            return true;
        }
    }

    dispatch_focus_event(*node, EventType::Focus);
    return true;
}

void FocusManager::clear_focus() {
    if (focused_ == nullptr) {
        return;
    }

    Node* previous = focused_;
    focused_ = nullptr;
    dispatch_focus_event(*previous, EventType::Blur);
}

bool FocusManager::move_focus(Node& root, bool forward) {
    root_ = &root;

    std::vector<Node*> order;
    collect_focusable(root, order);
    if (order.empty()) {
        return false;
    }

    const auto current = std::find(order.begin(), order.end(), focused_);
    std::size_t index = 0;
    if (current == order.end()) {
        index = forward ? 0 : order.size() - 1;
    } else {
        const std::size_t position = static_cast<std::size_t>(current - order.begin());
        index =
            forward ? (position + 1) % order.size() : (position + order.size() - 1) % order.size();
    }

    return request_focus(order[index]);
}

bool FocusManager::focus_next(Node& root) {
    return move_focus(root, true);
}

bool FocusManager::focus_previous(Node& root) {
    return move_focus(root, false);
}

} // namespace tessera::ui
