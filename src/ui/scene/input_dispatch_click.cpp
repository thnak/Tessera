// SPDX-License-Identifier: MIT

// Click synthesis wiring of the InputRouter (issue #6); the rules live in ClickSynthesizer.

#include "tessera/ui/scene/input_dispatch.hpp"

namespace tessera::ui {
namespace {

void fill_pointer(Event& event, const InputEvent& input) {
    event.x = input.x;
    event.y = input.y;
    event.button = input.button;
    event.pointer_id = input.pointer_id;
    event.pointer_kind = input.pointer_kind;
    event.pressure = input.pressure;
    event.timestamp_s = input.timestamp_s;
    event.is_primary = input.is_primary;
    event.modifiers = input.modifiers;
}

} // namespace

bool InputRouter::dispatch_click(Node& target, ClickSource source, const InputEvent* input) {
    Event event;
    event.type = EventType::Click;
    event.click_source = source;
    if (input != nullptr) {
        fill_pointer(event, *input);
    }
    if (source != ClickSource::Pointer) {
        // Keyboard and accessibility activations have no pointer: click at the node center with the
        // primary button, as the ClickEvent schema documents.
        const rhi::Rect bounds = target.world_bounds();
        event.x = bounds.x + bounds.width * 0.5f;
        event.y = bounds.y + bounds.height * 0.5f;
        event.button = PointerButton::Left;
        event.is_primary = true;
        if (input == nullptr) {
            event.modifiers = Modifier::None;
        }
    }
    return dispatch_event_to(target, event);
}

void InputRouter::synthesize_pointer_click(Node& root, const InputEvent& input) {
    ClickSynthesizer::Press press;
    if (!clicks_.take(input.pointer_id, press) || press.exceeded_slop ||
        !ClickSynthesizer::button_can_click(input.button)) {
        return;
    }
    // The pressed node may have left the scene during the press; a stale entry never clicks.
    Node* const pressed = resolve(CaptureEntry{press.pointer_id, press.node_id, press.node});
    if (pressed == nullptr) {
        return;
    }
    Node* const released = root.hit_test_deep(input.x, input.y);
    if (Node* const target = ClickSynthesizer::common_ancestor(pressed, released)) {
        (void)dispatch_click(*target, ClickSource::Pointer, &input);
    }
}

void InputRouter::synthesize_key_click(const InputEvent& input, bool consumed) {
    Node* const focused = focus_manager_.focused();
    if (clicks_.key(input.type, input.key, input.repeat, input.modifiers, focused, consumed)) {
        (void)dispatch_click(*focused, ClickSource::Keyboard, &input);
    }
}

bool InputRouter::invoke(Node& root, Node& target) {
    root_ = &root;
    return dispatch_click(target, ClickSource::Accessibility, nullptr);
}

} // namespace tessera::ui
