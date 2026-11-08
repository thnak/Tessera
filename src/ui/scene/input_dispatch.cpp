// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/input_dispatch.hpp"

namespace tessera::ui {

InputDispatchResult dispatch_input(Node& root, const InputEvent& input) {
    InputDispatchResult result;

    switch (input.type) {
    case InputEventType::PointerDown:
        result.type = EventType::PointerDown;
        break;
    case InputEventType::PointerUp:
        result.type = EventType::PointerUp;
        break;
    case InputEventType::PointerMove:
        result.type = EventType::PointerMove;
        break;
    default:
        return result;
    }

    Node* target = root.hit_test_deep(input.x, input.y);
    if (target == nullptr) {
        return result;
    }
    result.target = target;

    Event event;
    event.type = result.type;
    event.x = input.x;
    event.y = input.y;
    result.dispatched = dispatch_event(root, event);
    return result;
}

} // namespace tessera::ui
