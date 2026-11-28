// SPDX-License-Identifier: MIT

// Click synthesis wiring of the InputRouter (issue #6); the rules live in ClickSynthesizer.

#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"

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

std::string_view aria_value(const Node& node, std::string_view name) {
    for (const AriaAttribute& pair : node.aria_attributes()) {
        if (pair.name == name) {
            return pair.value;
        }
    }
    return {};
}

// First node under `node`, in tree order, whose `id` is `id`.
Node* find_by_element_id(Node& node, std::string_view id) {
    if (node.element_id() == id) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (Node* found = find_by_element_id(*child, id)) {
            return found;
        }
    }
    return nullptr;
}

// What a label's click activates once the control has focus: the controls whose own click is their
// meaning. A text field just takes focus, as in a browser.
bool activates_on_click(const Node& control) {
    const std::string_view role = control.role();
    return control.element_kind() == "Button" || role == "button" || role == "checkbox" ||
           role == "radio" || role == "switch" || role == "link";
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
    const bool dispatched = dispatch_event_to(target, event);
    if (dispatched && !event.stopped && !in_default_action_) {
        run_click_default_action(target);
    }
    return dispatched;
}

void InputRouter::run_click_default_action(Node& target) {
    // The nearest link or label around the click: a click on a span's parent, or on an icon inside
    // the link, still follows it.
    for (Node* node = &target; node != nullptr; node = node->parent()) {
        if (node->role() == "link") {
            const std::string_view href = aria_value(*node, "aria-href");
            if (!href.empty()) {
                if (link_handler_) {
                    in_default_action_ = true;
                    link_handler_(*node, href);
                    in_default_action_ = false;
                }
                return;
            }
        }
        const std::string_view control_id = aria_value(*node, "aria-for");
        if (control_id.empty() || root_ == nullptr) {
            continue;
        }
        Node* const control = find_by_element_id(*root_, control_id);
        if (control == nullptr || control == node) {
            return;
        }
        in_default_action_ = true;
        (void)focus_manager_.request_focus(control);
        if (activates_on_click(*control)) {
            (void)dispatch_click(*control, ClickSource::Accessibility, nullptr);
        }
        in_default_action_ = false;
        return;
    }
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
