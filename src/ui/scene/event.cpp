// SPDX-License-Identifier: MIT

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace tessera::ui {

namespace {

// Typical scene depth fits the inline buffer, so an event never allocates; only a tree deeper than
// this takes one allocation for the whole dispatch (hot-path rule, issue #286 performance gate).
constexpr std::size_t kInlinePathDepth = 64;

bool carries_pointer_position(EventType type) {
    switch (type) {
    case EventType::PointerDown:
    case EventType::PointerUp:
    case EventType::PointerMove:
    case EventType::PointerOver:
    case EventType::PointerOut:
    case EventType::PointerEnter:
    case EventType::PointerLeave:
    case EventType::Click:
        return true;
    default:
        return false;
    }
}

// Generated bindings read `x`/`y` as node-local logical px (jsx-intrinsics-v1 common-and-events
// section 2), so a handler can turn a pointer position into a fraction of its own box (a slider
// track). Dispatch carries scene coordinates, which `Node::on_event` listeners keep; only the
// read-only copy a binding receives is mapped into the bound node's box space, through the same
// inverse world transform hit testing uses. A node without bindings pays nothing, and a singular
// transform leaves the scene position (nothing meaningful to map to).
void run_bindings_in_node_space(const Node& node, const Event& event) {
    if (node.event_bindings().empty()) {
        return;
    }
    rendering::rhi::Mat3 inverse;
    if (!carries_pointer_position(event.type) || !node.world_transform().try_inverse(inverse)) {
        node.run_event_bindings(event);
        return;
    }
    Event local = event;
    inverse.transform_point(event.x, event.y, local.x, local.y);
    node.run_event_bindings(local);
}

} // namespace

bool dispatch_event_to(Node& target, Event& event) {
    std::size_t depth = 0;
    for (const Node* node = &target; node != nullptr; node = node->parent()) {
        ++depth;
    }
    std::array<Node*, kInlinePathDepth> inline_path;
    std::vector<Node*> heap_path;
    Node** storage = inline_path.data();
    if (depth > kInlinePathDepth) {
        heap_path.resize(depth);
        storage = heap_path.data();
    }
    // Root first: the capture phase walks it forward, the bubble phase backward.
    std::size_t slot = depth;
    for (Node* node = &target; node != nullptr; node = node->parent()) {
        storage[--slot] = node;
    }
    const std::span<Node*> path(storage, depth);

    event.phase = EventPhase::Capture;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        path[i]->on_event(event);
        if (event.stopped) {
            return true;
        }
    }

    event.phase = EventPhase::Target;
    target.on_event(event);
    if (event.stopped) {
        return true;
    }
    // Generated bindings run after the node's own listeners and before bubbling. They receive a
    // read-only copy, so they cannot consume the event (docs/design/scene-graph.md section 6).
    run_bindings_in_node_space(target, event);
    if (event.stopped) {
        return true;
    }

    event.phase = EventPhase::Bubble;
    for (std::size_t i = path.size(); i-- > 1;) {
        path[i - 1]->on_event(event);
        if (event.stopped) {
            break;
        }
        // Click bubbles through generated bindings too: `onClick` on a Button must fire when the
        // pressed node is its label. Other event types keep Target-only bindings.
        if (event.type == EventType::Click) {
            run_bindings_in_node_space(*path[i - 1], event);
            if (event.stopped) {
                break;
            }
        }
    }

    return true;
}

bool dispatch_event(Node& root, Event& event) {
    Node* target = root.hit_test_deep(event.x, event.y);
    if (target == nullptr) {
        return false;
    }
    return dispatch_event_to(*target, event);
}

} // namespace tessera::ui
