// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <vector>

namespace tessera::ui {

bool dispatch_event(Node& root, Event& event) {
    Node* target = root.hit_test_deep(event.x, event.y);
    if (target == nullptr) {
        return false;
    }

    std::vector<Node*> path;
    for (Node* node = target; node != nullptr; node = node->parent()) {
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
    target->on_event(event);
    if (event.stopped) {
        return true;
    }

    event.phase = EventPhase::Bubble;
    for (std::size_t i = path.size(); i-- > 1;) {
        path[i - 1]->on_event(event);
        if (event.stopped) {
            break;
        }
    }

    return true;
}

} // namespace tessera::ui
