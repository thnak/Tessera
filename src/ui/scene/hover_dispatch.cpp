// SPDX-License-Identifier: MIT

#include "hover_dispatch.hpp"

#include <cstddef>

namespace tessera::ui {
namespace {

Event boundary_event(EventType type, const InputEvent& input) {
    Event event;
    event.type = type;
    event.x = input.x;
    event.y = input.y;
    event.button = input.button;
    event.pointer_id = input.pointer_id;
    event.pointer_kind = input.pointer_kind;
    event.pressure = input.pressure;
    event.timestamp_s = input.timestamp_s;
    event.is_primary = input.is_primary;
    event.modifiers = input.modifiers;
    return event;
}

std::size_t depth_of(const Node* node) {
    std::size_t depth = 0;
    for (const Node* current = node->parent(); current != nullptr; current = current->parent()) {
        ++depth;
    }
    return depth;
}

// Deepest shared node of the two ancestor chains, or nullptr when either side has no target.
Node* common_ancestor(Node* lhs, Node* rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return nullptr;
    }
    std::size_t lhs_depth = depth_of(lhs);
    std::size_t rhs_depth = depth_of(rhs);
    Node* lhs_node = lhs;
    Node* rhs_node = rhs;
    while (lhs_depth > rhs_depth) {
        lhs_node = lhs_node->parent();
        --lhs_depth;
    }
    while (rhs_depth > lhs_depth) {
        rhs_node = rhs_node->parent();
        --rhs_depth;
    }
    while (lhs_node != rhs_node && lhs_node != nullptr && rhs_node != nullptr) {
        lhs_node = lhs_node->parent();
        rhs_node = rhs_node->parent();
    }
    return lhs_node == rhs_node ? lhs_node : nullptr;
}

// Bubbling boundary event: target phase on `target`, then bubble through the ancestors until
// (excluding) `exclusive`.
void dispatch_bubbling(Node& target, const Node* exclusive, EventType type,
                       const InputEvent& input) {
    Event event = boundary_event(type, input);
    event.phase = EventPhase::Target;
    target.on_event(event);
    if (event.stopped) {
        return;
    }
    event.phase = EventPhase::Bubble;
    for (Node* node = target.parent(); node != nullptr && node != exclusive;
         node = node->parent()) {
        node->on_event(event);
        if (event.stopped) {
            break;
        }
    }
}

void dispatch_target_only(Node& node, EventType type, const InputEvent& input) {
    Event event = boundary_event(type, input);
    event.phase = EventPhase::Target;
    node.on_event(event);
}

// Non-bubbling enter events: recurse to the root first so the outermost ancestor fires before its
// descendants, stopping strictly below `exclusive`.
void dispatch_enter_chain(Node& node, const Node* exclusive, const InputEvent& input) {
    if (&node != exclusive && node.parent() != nullptr) {
        dispatch_enter_chain(*node.parent(), exclusive, input);
    }
    if (&node != exclusive) {
        dispatch_target_only(node, EventType::PointerEnter, input);
    }
}

} // namespace

void dispatch_hover_transition(Node* old_target, Node* new_target, const InputEvent& input) {
    if (old_target == new_target) {
        return;
    }
    Node* const common = common_ancestor(old_target, new_target);
    // A side whose target is the common ancestor was already entered/left by the other side.
    if (old_target != nullptr && old_target != common) {
        // Leaving: out on the old target (bubbles to the common ancestor, exclusive)...
        dispatch_bubbling(*old_target, common, EventType::PointerOut, input);
        // ...then leave once per old-chain node below it, innermost first.
        for (Node* node = old_target; node != nullptr && node != common; node = node->parent()) {
            dispatch_target_only(*node, EventType::PointerLeave, input);
        }
    }
    if (new_target != nullptr && new_target != common) {
        // Entering: over on the new target (bubbles to the common ancestor, exclusive)...
        dispatch_bubbling(*new_target, common, EventType::PointerOver, input);
        // ...then enter once per new-chain node below it, outermost first.
        dispatch_enter_chain(*new_target, common, input);
    }
}

} // namespace tessera::ui
