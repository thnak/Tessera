// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/scroll.hpp"

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

void collect_focusable(Node& node, std::vector<Node*>& tree_order) {
    if (!node.visible()) {
        return;
    }
    if (node.focusable()) {
        tree_order.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_focusable(*child, tree_order);
    }
}

// Positive tab_index values come first, ascending; equal values keep tree order because the
// candidate list was collected in pre-order and the sort is stable. tab_index == 0 follows in
// tree order. Negative values are intentionally absent: they skip sequential traversal but remain
// valid request_focus targets.
std::vector<Node*> tab_order(Node& root) {
    std::vector<Node*> tree_order;
    collect_focusable(root, tree_order);

    std::vector<Node*> ordered;
    ordered.reserve(tree_order.size());
    for (Node* node : tree_order) {
        if (node->tab_index() > 0) {
            ordered.push_back(node);
        }
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const Node* lhs, const Node* rhs) {
        return lhs->tab_index() < rhs->tab_index();
    });
    for (Node* node : tree_order) {
        if (node->tab_index() == 0) {
            ordered.push_back(node);
        }
    }
    return ordered;
}

bool visible_chain(const Node& node) {
    for (const Node* current = &node; current != nullptr; current = current->parent()) {
        if (!current->visible()) {
            return false;
        }
    }
    return true;
}

bool contains(const Node& ancestor, const Node& node) {
    for (const Node* current = &node; current != nullptr; current = current->parent()) {
        if (current == &ancestor) {
            return true;
        }
    }
    return false;
}

Node* find_node_in(Node& node, NodeId id) {
    if (node.id() == id) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (Node* found = find_node_in(*child, id)) {
            return found;
        }
    }
    return nullptr;
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
    return node.focusable() && visible_chain(node) && is_attached(node) && is_within_modal(node);
}

Node* FocusManager::find_node(NodeId id) const {
    if (root_ == nullptr) {
        return nullptr;
    }
    return find_node_in(*root_, id);
}

Node* FocusManager::modal_root() const {
    if (modal_root_ == nullptr) {
        return nullptr;
    }
    if (root_ == nullptr) {
        return modal_root_;
    }
    return find_node(modal_root_id_);
}

bool FocusManager::allows_focus(const Node& node) const {
    const Node* trap = modal_root();
    return trap == nullptr || contains(*trap, node);
}

bool FocusManager::is_within_modal(const Node& node) const {
    const Node* trap = modal_root();
    return trap == nullptr || contains(*trap, node);
}

void FocusManager::refresh_state() {
    if (root_ == nullptr) {
        return;
    }
    if (focused_ != nullptr) {
        if (Node* resolved = find_node(focused_id_)) {
            focused_ = resolved;
        } else {
            focused_ = nullptr;
            focused_id_ = 0;
        }
    }
    if (saved_focus_ != nullptr) {
        if (Node* resolved = find_node(saved_focus_id_)) {
            saved_focus_ = resolved;
        } else {
            saved_focus_ = nullptr;
            saved_focus_id_ = 0;
        }
    }
    if (modal_root_ != nullptr) {
        if (Node* resolved = find_node(modal_root_id_)) {
            modal_root_ = resolved;
        } else {
            release_modal_trap();
        }
    }
}

void FocusManager::release_modal_trap() {
    Node* restore = saved_focus_;
    const NodeId restore_id = saved_focus_id_;
    modal_root_ = nullptr;
    modal_root_id_ = 0;
    saved_focus_ = nullptr;
    saved_focus_id_ = 0;
    if (restore == nullptr) {
        return;
    }
    if (root_ != nullptr) {
        restore = find_node(restore_id);
    }
    if (restore != nullptr) {
        request_focus(restore);
    }
}

bool FocusManager::focus_first_in_modal() {
    if (modal_root_ == nullptr) {
        return false;
    }
    const std::vector<Node*> order = tab_order(*modal_root_);
    if (order.empty()) {
        clear_focus();
        return false;
    }
    return request_focus(order.front());
}

bool FocusManager::request_focus(Node* node) {
    refresh_state();
    if (node == nullptr || !is_valid(*node)) {
        return false;
    }
    if (focused_ == node) {
        node->set_pseudo_state(style::PseudoState::Focus, true);
        // A programmatic request has no input modality, so the keyboard ring is cleared unless the
        // caller re-enables it. This also covers a pointer press on the already-focused node.
        node->set_pseudo_state(style::PseudoState::FocusVisible, false);
        // Re-requesting focus also re-reveals the node if the scene scrolled it out of view.
        scroll_into_view(*node);
        return true;
    }

    Node* previous = focused_;
    if (previous != nullptr) {
        previous->set_pseudo_state(style::PseudoState::Focus, false);
        previous->set_pseudo_state(style::PseudoState::FocusVisible, false);
    }
    focused_ = node;
    focused_id_ = node->id();
    if (previous != nullptr) {
        dispatch_focus_event(*previous, EventType::Blur);
        // A Blur handler may have moved focus again; do not overwrite that decision.
        if (focused_ != node) {
            return true;
        }
    }

    node->set_pseudo_state(style::PseudoState::Focus, true);
    // Clearing before the Focus event lets the router decide the modality afterwards.
    node->set_pseudo_state(style::PseudoState::FocusVisible, false);
    dispatch_focus_event(*node, EventType::Focus);
    // Reveal the node in every scrollable ancestor (innermost to outermost) after the Focus event
    // so handlers that moved or resized it are taken into account. Already-visible nodes change no
    // offset; this runs for programmatic requests and keyboard traversal alike.
    scroll_into_view(*node);
    return true;
}

void FocusManager::set_focus_visible(bool visible) {
    refresh_state();
    if (focused_ != nullptr) {
        focused_->set_pseudo_state(style::PseudoState::FocusVisible, visible);
    }
}

void FocusManager::clear_focus() {
    refresh_state();
    if (focused_ == nullptr) {
        return;
    }

    Node* previous = focused_;
    focused_ = nullptr;
    focused_id_ = 0;
    previous->set_pseudo_state(style::PseudoState::Focus, false);
    previous->set_pseudo_state(style::PseudoState::FocusVisible, false);
    dispatch_focus_event(*previous, EventType::Blur);
}

bool FocusManager::set_modal_root(Node* node) {
    refresh_state();
    if (node == nullptr) {
        clear_modal_root();
        return true;
    }
    if (!is_attached(*node) || !visible_chain(*node)) {
        return false;
    }

    if (modal_root_ != nullptr) {
        if (modal_root_ == node) {
            return true;
        }
        // Keep the original pre-modal focus unless the new trap contains it.
        if (saved_focus_ != nullptr && contains(*node, *saved_focus_)) {
            saved_focus_ = nullptr;
            saved_focus_id_ = 0;
        }
    } else if (focused_ != nullptr && !contains(*node, *focused_)) {
        saved_focus_ = focused_;
        saved_focus_id_ = focused_id_;
    } else {
        saved_focus_ = nullptr;
        saved_focus_id_ = 0;
    }

    modal_root_ = node;
    modal_root_id_ = node->id();
    if (focused_ == nullptr || !contains(*node, *focused_)) {
        focus_first_in_modal();
    }
    return true;
}

void FocusManager::clear_modal_root() {
    refresh_state();
    if (modal_root_ == nullptr) {
        return;
    }
    release_modal_trap();
}

void FocusManager::reset() {
    // Forget the trap before clearing focus so refresh_state cannot restore the saved target.
    modal_root_ = nullptr;
    modal_root_id_ = 0;
    saved_focus_ = nullptr;
    saved_focus_id_ = 0;
    clear_focus();
}

bool FocusManager::move_focus(Node& root, bool forward) {
    root_ = &root;
    refresh_state();

    Node* trap = modal_root();
    Node& base = trap != nullptr ? *trap : root;
    const std::vector<Node*> order = tab_order(base);
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
