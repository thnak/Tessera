// SPDX-License-Identifier: MIT

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

namespace tessera::ui {
namespace {

// Pre-order lookup used to resolve captured/hovered/active targets by id, so a node removed from
// the scene can never be dereferenced through a stale raw pointer.
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
    // Captures resolve through the root of the scene currently receiving input.
    root_ = &root;

    InputDispatchResult result;
    if (input.type == InputEventType::PointerCancel) {
        // A cancelled sequence only ends that pointer; hover tracks the pointer position and is
        // left alone until the next move. Scene focus is not touched.
        if (input.pointer_id == primary_pointer_id_) {
            clear_active();
        }
        remove_capture(input.pointer_id);
        promote_primary_after(input.pointer_id);
        return result;
    }
    if (input.type == InputEventType::Blur || input.type == InputEventType::Focus) {
        // Window focus changes cancel every in-flight press and drop hover/active: no pointer can
        // be considered over the scene any more. Scene Focus/Blur are owned by FocusManager and
        // are not synthesized here.
        clear_captures();
        clear_hover();
        clear_active();
        primary_pointer_id_ = 0;
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
        // The first press while no pointer is captured becomes the primary pointer; it drives the
        // hover/active pseudo-states for the whole press.
        if (capture_count_ == 0) {
            primary_pointer_id_ = input.pointer_id;
        }
        const bool primary = input.pointer_id == primary_pointer_id_;
        Node* target = root.hit_test_deep(input.x, input.y);
        if (target == nullptr) {
            release_pointer_capture(input.pointer_id);
            // A press that lands outside the scene cannot stay active on an older target.
            if (primary) {
                clear_active();
            }
            // A click outside the scene must not break an active modal trap.
            if (focus_manager_.modal_root() == nullptr) {
                focus_manager_.clear_focus();
            }
            return result;
        }
        if (primary) {
            set_active(target);
        }
        // Bounded implicit capture; a press beyond the bound still dispatches uncaptured.
        set_pointer_capture(target, input.pointer_id);
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
        const bool primary = input.pointer_id == primary_pointer_id_;
        Node* target = take_capture_target(input.pointer_id);
        Node* hit = nullptr;
        // Only the primary pointer updates hover; hit testing is still needed when a secondary
        // pointer is not captured.
        if (primary || target == nullptr) {
            hit = root.hit_test_deep(input.x, input.y);
        }
        if (primary) {
            set_hover(hit);
        }
        if (target == nullptr) {
            target = hit;
        }
        if (target == nullptr) {
            return result;
        }
        Event event = make_pointer_event(type, input);
        return dispatch_to_target(*target, event);
    }

    // PointerUp: deliver to that pointer's capture target, else the node under the cursor, then
    // end the press. The active bit is cleared before dispatch so `:active` never outlives the
    // button; the capture is released after dispatch, as before.
    const bool primary = input.pointer_id == primary_pointer_id_;
    Node* target = take_capture_target(input.pointer_id);
    if (target == nullptr) {
        target = root.hit_test_deep(input.x, input.y);
    }
    if (primary) {
        clear_active();
    }
    InputDispatchResult dispatched;
    if (target != nullptr) {
        Event event = make_pointer_event(type, input);
        dispatched = dispatch_to_target(*target, event);
    }
    remove_capture(input.pointer_id);
    promote_primary_after(input.pointer_id);
    return dispatched;
}

bool InputRouter::set_pointer_capture(Node* node, std::uint32_t pointer_id) {
    if (node == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < capture_count_; ++i) {
        if (captures_[i].pointer_id == pointer_id) {
            captures_[i].node = node;
            captures_[i].node_id = node->id();
            return true;
        }
    }
    if (capture_count_ == kMaxPointerCaptures) {
        return false;
    }
    captures_[capture_count_++] = CaptureEntry{pointer_id, node->id(), node};
    return true;
}

void InputRouter::release_pointer_capture(std::uint32_t pointer_id) {
    remove_capture(pointer_id);
}

Node* InputRouter::captured(std::uint32_t pointer_id) const {
    for (std::size_t i = 0; i < capture_count_; ++i) {
        if (captures_[i].pointer_id == pointer_id) {
            return resolve(captures_[i]);
        }
    }
    return nullptr;
}

Node* InputRouter::resolve(const CaptureEntry& entry) const {
    if (entry.node == nullptr) {
        return nullptr;
    }
    // Before the router has seen a scene root there is nothing to resolve against; the caller owns
    // the node's lifetime in that window.
    if (root_ == nullptr) {
        return entry.node;
    }
    Node* found = find_node_in(*root_, entry.node_id);
    // A different node means the id was reused; treat the stored pointer as stale rather than
    // risking a dereference of the old node.
    return found == entry.node ? found : nullptr;
}

Node* InputRouter::take_capture_target(std::uint32_t pointer_id) {
    for (std::size_t i = 0; i < capture_count_; ++i) {
        if (captures_[i].pointer_id != pointer_id) {
            continue;
        }
        Node* target = resolve(captures_[i]);
        if (target == nullptr) {
            remove_capture(pointer_id);
        }
        return target;
    }
    return nullptr;
}

void InputRouter::remove_capture(std::uint32_t pointer_id) {
    for (std::size_t i = 0; i < capture_count_; ++i) {
        if (captures_[i].pointer_id != pointer_id) {
            continue;
        }
        for (std::size_t j = i + 1; j < capture_count_; ++j) {
            captures_[j - 1] = captures_[j];
        }
        --capture_count_;
        captures_[capture_count_] = CaptureEntry{};
        return;
    }
}

void InputRouter::clear_captures() {
    captures_ = {};
    capture_count_ = 0;
}

void InputRouter::promote_primary_after(std::uint32_t pointer_id) {
    if (pointer_id != primary_pointer_id_) {
        return;
    }
    // Insertion order keeps the hand-off deterministic; pointer 0 is the default mouse id used
    // before any press.
    primary_pointer_id_ = capture_count_ > 0 ? captures_[0].pointer_id : 0;
}

void InputRouter::set_hover(Node* target) {
    if (target == hovered_ && (target == nullptr || target->id() == hovered_id_)) {
        return;
    }
    clear_hover();
    if (target == nullptr) {
        return;
    }
    hovered_ = target;
    hovered_id_ = target->id();
    target->set_pseudo_state(style::PseudoState::Hover, true);
}

void InputRouter::clear_hover() {
    if (hovered_ != nullptr) {
        // Resolve before clearing so a target removed from the scene is never dereferenced.
        Node* live = root_ != nullptr ? find_node_in(*root_, hovered_id_) : nullptr;
        if (live == hovered_) {
            live->set_pseudo_state(style::PseudoState::Hover, false);
        }
    }
    hovered_ = nullptr;
    hovered_id_ = 0;
}

void InputRouter::set_active(Node* target) {
    if (target == active_ && (target == nullptr || target->id() == active_id_)) {
        return;
    }
    clear_active();
    if (target == nullptr) {
        return;
    }
    active_ = target;
    active_id_ = target->id();
    target->set_pseudo_state(style::PseudoState::Active, true);
}

void InputRouter::clear_active() {
    if (active_ != nullptr) {
        Node* live = root_ != nullptr ? find_node_in(*root_, active_id_) : nullptr;
        if (live == active_) {
            live->set_pseudo_state(style::PseudoState::Active, false);
        }
    }
    active_ = nullptr;
    active_id_ = 0;
}

void InputRouter::reset() {
    clear_captures();
    clear_hover();
    clear_active();
    primary_pointer_id_ = 0;
    root_ = nullptr;
    focus_manager_.reset();
}

} // namespace tessera::ui
