// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/scroll.hpp"
#include "wheel_dispatch.hpp"

namespace tessera::ui {
namespace {

Event make_wheel_event(const InputEvent& input) {
    Event event;
    event.type = EventType::Wheel;
    event.x = input.x;
    event.y = input.y;
    event.button = input.button;
    event.pointer_id = input.pointer_id;
    event.pointer_kind = input.pointer_kind;
    event.pressure = input.pressure;
    event.timestamp_s = input.timestamp_s;
    event.is_primary = input.is_primary;
    event.modifiers = input.modifiers;
    // Win32 already reports wheel deltas in `dx`/`dy` with the web sign.
    event.scroll_x = input.dx;
    event.scroll_y = input.dy;
    return event;
}

} // namespace

InputDispatchResult dispatch_wheel(Node& root, const InputEvent& input) {
    InputDispatchResult result;
    result.type = EventType::Wheel;

    Node* const target = root.hit_test_deep(input.x, input.y);
    if (target == nullptr) {
        return result;
    }

    Event event = make_wheel_event(input);
    result.target = target;
    result.target_id = target->id();
    result.dispatched = dispatch_event_to(*target, event);
    if (!event.prevent_default) {
        // Default action: the innermost scroll container consumes the wheel and the remainder
        // chains outward while each container's `overscroll-behavior` allows it. The deltas are
        // converted from platform wheel units to logical pixels.
        apply_wheel_scroll(*target, event.scroll_x, event.scroll_y);
    }
    return result;
}

} // namespace tessera::ui
