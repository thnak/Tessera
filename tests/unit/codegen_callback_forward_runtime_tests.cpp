// SPDX-License-Identifier: MIT

// Runtime proof of callback forwarding (issue #286): the generated `ForwardWrap` forwards its
// `onPress` and `onText` props to the Button click and the TextInput change, `ForwardOuter` binds
// `onPress` to its own setter, and an unset callback is skipped. No GPU.

#include "ForwardCounter.hpp"
#include "ForwardOuter.hpp"
#include "ForwardWrap.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

ui::Node* find_kind(ui::Node& node, std::string_view kind) {
    if (node.element_kind() == kind) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (ui::Node* found = find_kind(*child, kind)) {
            return found;
        }
    }
    return nullptr;
}

ui::Event click_event() {
    ui::Event click;
    click.type = ui::EventType::Click;
    click.x = 4.0f;
    click.y = 4.0f;
    click.pointer_kind = ui::PointerKind::Mouse;
    click.button = ui::PointerButton::Left;
    return click;
}

// The caller's handler is the callback the wrapper forwards the click to.
void test_click_reaches_the_callers_handler() {
    forward::gen::ForwardOuter outer;
    ui::Node root(1000);
    outer.build(root);
    outer.apply_props();
    ui::Node* button = find_kind(root, "Button");
    check(button != nullptr, "the wrapped Button exists");
    if (button == nullptr) {
        return;
    }
    check(outer.state_count_ == 0, "the counter starts at zero");
    ui::Event click = click_event();
    ui::dispatch_event_to(*button, click);
    check(outer.state_count_ == 1, "the click ran the caller's handler through the wrapper");
    click = click_event();
    ui::dispatch_event_to(*button, click);
    check(outer.state_count_ == 2, "each click forwards once");
}

// A wrapper used without the callback never calls through an empty std::function.
void test_unset_callback_is_skipped() {
    forward::gen::ForwardWrap wrap;
    ui::Node root(2000);
    wrap.build(root);
    ui::Node* button = find_kind(root, "Button");
    check(button != nullptr, "the Button exists without callbacks");
    if (button == nullptr) {
        return;
    }
    ui::Event click = click_event();
    ui::dispatch_event_to(*button, click);
    check(true, "a click with no callback is a no-op, not a crash");
}

// A payload callback receives the event's payload record unchanged.
void test_payload_is_passed_on() {
    forward::gen::ForwardWrap wrap;
    std::string seen;
    wrap.prop_onText_ = [&seen](const ui::TextChangeEvent& event) { seen = event.value; };
    ui::Node root(3000);
    wrap.build(root);
    ui::Node* input = find_kind(root, "TextInput");
    check(input != nullptr, "the TextInput exists");
    if (input == nullptr) {
        return;
    }
    input->set_pseudo_state(ui::style::PseudoState::Focus);
    ui::Event text;
    text.type = ui::EventType::Text;
    text.text_codepoint = static_cast<std::uint32_t>('A');
    ui::dispatch_event_to(*input, text);
    check(seen == "xA",
          "the callback saw the committed text (controlled \"x\" plus the typed A): got \"" + seen +
              "\"");
}

// A handler body runs its setter and then calls the callback prop (option 2 of issue #286); the
// payload callback is called with the handler's own parameter.
void test_calls_from_a_handler_body() {
    forward::gen::ForwardCounter counter;
    int changed = 0;
    std::string seen;
    counter.prop_onChanged_ = [&changed]() { ++changed; };
    counter.prop_onText_ = [&seen](const ui::TextChangeEvent& event) { seen = event.value; };
    ui::Node root(4000);
    counter.build(root);
    ui::Node* button = find_kind(root, "Button");
    ui::Node* input = find_kind(root, "TextInput");
    check(button != nullptr && input != nullptr, "the counter has its Button and TextInput");
    if (button == nullptr || input == nullptr) {
        return;
    }
    ui::Event click = click_event();
    ui::dispatch_event_to(*button, click);
    check(counter.state_n_ == 1, "the setter ran");
    check(changed == 1, "the callback was called once after it");
    input->set_pseudo_state(ui::style::PseudoState::Focus);
    ui::Event text;
    text.type = ui::EventType::Text;
    text.text_codepoint = static_cast<std::uint32_t>('B');
    ui::dispatch_event_to(*input, text);
    check(seen == "xB", "the payload callback received the TextChangeEvent: got \"" + seen + "\"");
}

// With no callbacks set the same handlers still run their setters and never call through nothing.
void test_unset_callbacks_in_a_handler_body() {
    forward::gen::ForwardCounter counter;
    ui::Node root(5000);
    counter.build(root);
    ui::Node* button = find_kind(root, "Button");
    check(button != nullptr, "the Button exists");
    if (button == nullptr) {
        return;
    }
    ui::Event click = click_event();
    ui::dispatch_event_to(*button, click);
    check(counter.state_n_ == 1, "the setter still ran without a callback");
}

} // namespace

int main() {
    test_click_reaches_the_callers_handler();
    test_unset_callback_is_skipped();
    test_payload_is_passed_on();
    test_calls_from_a_handler_body();
    test_unset_callbacks_in_a_handler_body();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen callback forward runtime tests passed\n";
    return 0;
}
