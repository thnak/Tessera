// SPDX-License-Identifier: MIT

// Focus scope (trap and restore) and roving focus, issue #236. Both are driven by flags on scene
// nodes (`Node::focus_trap`, `Node::focus_group`) that the JSX props `focusTrap` and `focusGroup`
// set, so a behavior primitive in TSX needs no handler code for them.

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/focus.hpp"

#include <vector>

namespace tessera::ui {
namespace {

bool visible_chain(const Node& node) {
    for (const Node* current = &node; current != nullptr; current = current->parent()) {
        if (!current->visible()) {
            return false;
        }
    }
    return true;
}

// The last visible `focus_trap` node in pre-order, so a nested scope wins over its ancestor.
Node* last_trap(Node& node) {
    if (!node.visible()) {
        return nullptr;
    }
    Node* found = node.focus_trap() ? &node : nullptr;
    for (const auto& child : node.children()) {
        if (Node* inner = last_trap(*child)) {
            found = inner;
        }
    }
    return found;
}

void collect_items(Node& node, std::vector<Node*>& items) {
    if (!node.visible()) {
        return;
    }
    if (node.focusable()) {
        items.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_items(*child, items);
    }
}

// Whether `key` moves along the axis of `group`; `forward` and `edge` describe the move.
bool group_key(Node::FocusGroup group, KeyCode key, bool& forward, bool& edge) {
    const bool horizontal = group != Node::FocusGroup::Vertical;
    const bool vertical = group != Node::FocusGroup::Horizontal;
    edge = false;
    if (horizontal && key == key::kArrowRight) {
        forward = true;
    } else if (horizontal && key == key::kArrowLeft) {
        forward = false;
    } else if (vertical && key == key::kArrowDown) {
        forward = true;
    } else if (vertical && key == key::kArrowUp) {
        forward = false;
    } else if (key == key::kHome || key == key::kEnd) {
        forward = key == key::kHome; // Home is the first item, End the last
        edge = true;
    } else {
        return false;
    }
    return true;
}

} // namespace

void FocusManager::sync_focus_trap(Node& root) {
    root_ = &root;
    refresh_state();
    Node* trap = last_trap(root);
    if (trap != nullptr) {
        if (modal_root_ != trap && set_modal_root(trap)) {
            trap_owned_ = true;
        }
        // Focus left on a node a closed inner scope just hid goes back into the active scope.
        if (focused_ != nullptr && !visible_chain(*focused_)) {
            focus_first_in_modal();
        }
        return;
    }
    if (trap_owned_) {
        trap_owned_ = false;
        clear_modal_root();
    }
}

bool FocusManager::move_in_focus_group(Node& root, KeyCode key) {
    root_ = &root;
    refresh_state();
    if (focused_ == nullptr) {
        return false;
    }
    bool forward = true;
    bool edge = false;
    Node* group = nullptr;
    for (Node* current = focused_->parent(); current != nullptr; current = current->parent()) {
        if (current->focus_group() != Node::FocusGroup::None &&
            group_key(current->focus_group(), key, forward, edge)) {
            group = current;
            break;
        }
    }
    if (group == nullptr || !visible_chain(*group)) {
        return false;
    }
    std::vector<Node*> items;
    collect_items(*group, items);
    if (items.empty()) {
        return false;
    }
    std::size_t index = 0;
    if (edge) {
        index = forward ? 0 : items.size() - 1;
    } else {
        std::size_t at = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i] == focused_) {
                at = i;
            }
        }
        index = forward ? (at + 1) % items.size() : (at + items.size() - 1) % items.size();
    }
    Node* const before = focused_;
    return request_focus(items[index]) && focused_ != before;
}

} // namespace tessera::ui
