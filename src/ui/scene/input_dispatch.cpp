// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/input_dispatch.hpp"

namespace tessera::ui {
namespace {

bool pointer_event_type(InputEventType input, EventType& output) {
    switch (input) {
    case InputEventType::PointerDown:
        output = EventType::PointerDown;
        return true;
    case InputEventType::PointerUp:
        output = EventType::PointerUp;
        return true;
    case InputEventType::PointerMove:
        output = EventType::PointerMove;
        return true;
    default:
        return false;
    }
}

InputDispatchResult dispatch_to_target(Node& target, EventType type, float x, float y) {
    InputDispatchResult result;
    result.type = type;
    result.target = &target;

    Event event;
    event.type = type;
    event.x = x;
    event.y = y;
    result.dispatched = dispatch_event_to(target, event);
    return result;
}

} // namespace

InputDispatchResult dispatch_input(Node& root, const InputEvent& input) {
    InputDispatchResult result;
    EventType type = EventType::PointerMove;
    if (!pointer_event_type(input.type, type)) {
        return result;
    }
    result.type = type;

    Node* target = root.hit_test_deep(input.x, input.y);
    if (target == nullptr) {
        return result;
    }
    return dispatch_to_target(*target, type, input.x, input.y);
}

InputDispatchResult InputRouter::dispatch(Node& root, const InputEvent& input) {
    InputDispatchResult result;
    if (input.type == InputEventType::PointerCancel) {
        captured_ = nullptr;
        return result;
    }

    EventType type = EventType::PointerMove;
    if (!pointer_event_type(input.type, type)) {
        return result;
    }
    result.type = type;

    if (input.type == InputEventType::PointerDown) {
        Node* target = root.hit_test_deep(input.x, input.y);
        if (target == nullptr) {
            captured_ = nullptr;
            return result;
        }
        captured_ = target;
        return dispatch_to_target(*target, type, input.x, input.y);
    }

    if (input.type == InputEventType::PointerMove && captured_ != nullptr) {
        return dispatch_to_target(*captured_, type, input.x, input.y);
    }

    if (input.type == InputEventType::PointerUp) {
        Node* target = captured_ != nullptr ? captured_ : root.hit_test_deep(input.x, input.y);
        if (target == nullptr) {
            captured_ = nullptr;
            return result;
        }
        const InputDispatchResult dispatched = dispatch_to_target(*target, type, input.x, input.y);
        captured_ = nullptr;
        return dispatched;
    }

    return dispatch_input(root, input);
}

void InputRouter::reset() {
    captured_ = nullptr;
}

} // namespace tessera::ui
