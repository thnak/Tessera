// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <vector>

namespace tessera::ui {

bool dispatch_event_to(Node& target, Event& event) {
    std::vector<Node*> path;
    for (Node* node = &target; node != nullptr; node = node->parent()) {
        path.push_back(node);
    }
    std::reverse(path.begin(), path.end());

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
    target.run_event_bindings(event);
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
            path[i - 1]->run_event_bindings(event);
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
