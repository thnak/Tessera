// SPDX-License-Identifier: MIT

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

namespace tessera::ui {
namespace {

bool key_event_type(InputEventType input, EventType& output) {
    switch (input) {
    case InputEventType::KeyDown:
        output = EventType::KeyDown;
        return true;
    case InputEventType::KeyUp:
        output = EventType::KeyUp;
        return true;
    default:
        return false;
    }
}

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

void fill_pointer_fields(Event& event, const InputEvent& input) {
    event.x = input.x;
    event.y = input.y;
    event.button = input.button;
    event.pointer_id = input.pointer_id;
    event.pressure = input.pressure;
    event.timestamp_s = input.timestamp_s;
    event.is_primary = input.is_primary;
    event.modifiers = input.modifiers;
}

Event make_pointer_event(EventType type, const InputEvent& input) {
    Event event;
    event.type = type;
    fill_pointer_fields(event, input);
    return event;
}

Event make_wheel_event(const InputEvent& input) {
    Event event = make_pointer_event(EventType::Wheel, input);
    // Win32 already reports wheel deltas in `dx`/`dy` with the web sign.
    event.scroll_x = input.dx;
    event.scroll_y = input.dy;
    return event;
}

Event make_key_event(EventType type, const InputEvent& input) {
    Event event;
    event.type = type;
    fill_pointer_fields(event, input);
    event.key = input.key;
    event.text_codepoint = input.text_codepoint;
    event.repeat = input.repeat;
    return event;
}

Event make_text_event(const InputEvent& input) {
    Event event;
    event.type = EventType::Text;
    fill_pointer_fields(event, input);
    event.text_codepoint = input.text_codepoint;
    return event;
}

InputDispatchResult dispatch_to_target(Node& target, Event& event) {
    InputDispatchResult result;
    result.type = event.type;
    result.target = &target;
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
    Event event = make_pointer_event(type, input);
    return dispatch_to_target(*target, event);
}

InputDispatchResult InputRouter::dispatch(Node& root, const InputEvent& input) {
    InputDispatchResult result;
    if (input.type == InputEventType::PointerCancel) {
        captured_ = nullptr;
        return result;
    }
    if (input.type == InputEventType::Blur || input.type == InputEventType::Focus) {
        // Window focus changes cancel an in-flight press. Scene Focus/Blur are owned by
        // FocusManager and are not synthesized here.
        captured_ = nullptr;
        return result;
    }

    // Key, text, and sequential focus all need the root to resolve the focused node.
    focus_manager_.set_root(root);

    EventType type = EventType::PointerMove;
    if (key_event_type(input.type, type)) {
        result.type = type;
        bool prevent_default = false;
        if (Node* target = focus_manager_.focused()) {
            Event event = make_key_event(type, input);
            result = dispatch_to_target(*target, event);
            prevent_default = event.prevent_default;
        }

        // Default action: Tab moves sequential focus unless a handler prevented it. It runs even
        // with no focused node, because FocusManager starts traversal at the first (or last)
        // candidate; a key event itself still needs a focus target to be dispatched. The modal
        // trap is enforced inside FocusManager.
        if (type == EventType::KeyDown && input.key == key::kTab) {
            if (prevent_default) {
                result.default_prevented = true;
            } else {
                Node* const before = focus_manager_.focused();
                const bool cycled = has_modifier(input.modifiers, Modifier::Shift)
                                        ? focus_manager_.focus_previous(root)
                                        : focus_manager_.focus_next(root);
                // A single focus candidate wraps to itself; report only an actual change.
                result.focus_moved = cycled && focus_manager_.focused() != before;
            }
        }
        return result;
    }

    if (input.type == InputEventType::Text) {
        result.type = EventType::Text;
        Node* target = focus_manager_.focused();
        if (target == nullptr) {
            return result;
        }
        Event event = make_text_event(input);
        return dispatch_to_target(*target, event);
    }

    if (input.type == InputEventType::Wheel) {
        result.type = EventType::Wheel;
        Node* target = root.hit_test_deep(input.x, input.y);
        if (target == nullptr) {
            return result;
        }
        Event event = make_wheel_event(input);
        return dispatch_to_target(*target, event);
    }

    if (!pointer_event_type(input.type, type)) {
        return result;
    }
    result.type = type;

    if (input.type == InputEventType::PointerDown) {
        Node* target = root.hit_test_deep(input.x, input.y);
        if (target == nullptr) {
            captured_ = nullptr;
            // A click outside the scene must not break an active modal trap.
            if (focus_manager_.modal_root() == nullptr) {
                focus_manager_.clear_focus();
            }
            return result;
        }
        captured_ = target;
        if (target->focusable()) {
            // request_focus rejects targets outside the active modal subtree.
            focus_manager_.request_focus(target);
        } else if (focus_manager_.allows_focus(*target)) {
            focus_manager_.clear_focus();
        }
        Event event = make_pointer_event(type, input);
        return dispatch_to_target(*target, event);
    }

    if (input.type == InputEventType::PointerMove) {
        Node* target = captured_ != nullptr ? captured_ : root.hit_test_deep(input.x, input.y);
        if (target == nullptr) {
            return result;
        }
        Event event = make_pointer_event(type, input);
        return dispatch_to_target(*target, event);
    }

    // PointerUp: deliver to the capture target, else the node under the cursor, then release.
    Node* target = captured_ != nullptr ? captured_ : root.hit_test_deep(input.x, input.y);
    if (target == nullptr) {
        captured_ = nullptr;
        return result;
    }
    Event event = make_pointer_event(type, input);
    const InputDispatchResult dispatched = dispatch_to_target(*target, event);
    captured_ = nullptr;
    return dispatched;
}

void InputRouter::reset() {
    captured_ = nullptr;
    focus_manager_.reset();
}

} // namespace tessera::ui
