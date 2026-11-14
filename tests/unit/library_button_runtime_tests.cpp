// SPDX-License-Identifier: MIT

// Headless interaction test of the library Button (issue #239, ADR-047 item 10): the generated
// `ButtonDemo` instantiates `<Button>` three times (enabled, disabled, small danger), and keyboard
// and pointer input through the real `InputRouter` reach the caller's `onPress`. No GPU, no clock.

#include "ButtonDemo.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

namespace ui = tessera::ui;
namespace key = tessera::ui::key;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void collect_buttons(ui::Node& node, std::vector<ui::Node*>& out) {
    if (node.focusable() && node.role() == "button") {
        out.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_buttons(*child, out);
    }
}

// Every ancestor covers the window at the origin, so a Button's bounds are window coordinates.
void cover_window(ui::Node& node) {
    node.set_bounds({0, 0, 400, 200});
    for (const auto& child : node.children()) {
        cover_window(*child);
    }
}

ui::InputEvent keyboard(ui::InputEventType type, ui::KeyCode code) {
    ui::InputEvent event;
    event.type = type;
    event.key = code;
    return event;
}

ui::InputEvent pointer(ui::InputEventType type, float x, float y) {
    ui::InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.pointer_id = 1;
    event.pointer_kind = ui::PointerKind::Mouse;
    event.button = ui::PointerButton::Left;
    event.is_primary = true;
    return event;
}

bool has_class(const ui::Node& node, const std::string& name) {
    const std::string classes = " " + std::string(node.class_name()) + " ";
    return classes.find(" " + name + " ") != std::string::npos;
}

struct Fixture {
    Fixture() {
        demo.build(root);
        demo.apply_props();
        collect_buttons(root, buttons);
        cover_window(root);
        float y = 10.0f;
        for (ui::Node* button : buttons) {
            // The Component host wraps the Button's root; both cover exactly the Button, so a hit
            // test at a Button's center finds that Button and no other.
            if (ui::Node* host = button->parent()) {
                host->set_bounds({10, y, 120, 32});
            }
            button->set_bounds({0, 0, 120, 32});
            for (const auto& child : button->children()) {
                child->set_bounds({0, 0, 120, 32});
            }
            y += 40.0f;
        }
        router.focus_manager().set_root(root);
    }

    ui::Node root{1000};
    button_demo::gen::ButtonDemo demo;
    std::vector<ui::Node*> buttons;
    ui::InputRouter router;
};

} // namespace

int main() {
    Fixture fixture;
    check(fixture.buttons.size() == 3, "three Buttons are focusable `button` nodes");
    // Instances of one component own distinct NodeIds (issue #295), or focus and input would
    // resolve to the first Button.
    check(fixture.buttons.size() == 3 && fixture.buttons[0]->id() != fixture.buttons[1]->id() &&
              fixture.buttons[1]->id() != fixture.buttons[2]->id() &&
              fixture.buttons[0]->id() != fixture.buttons[2]->id(),
          "each Button instance has its own NodeId");
    if (fixture.buttons.size() != 3) {
        return 1;
    }
    ui::Node& enabled = *fixture.buttons[0];
    ui::Node& disabled = *fixture.buttons[1];
    ui::Node& danger = *fixture.buttons[2];
    auto& demo = fixture.demo;
    auto& router = fixture.router;

    // Keyboard: Enter on the focused enabled Button calls onPress once.
    router.focus_manager().request_focus(&enabled);
    router.dispatch(fixture.root, keyboard(ui::InputEventType::KeyDown, key::kEnter));
    check(demo.state_count_ == 1, "Enter activates the enabled Button");
    check(demo.child_2_.state_pressed_, "Enter presses it");
    router.dispatch(fixture.root, keyboard(ui::InputEventType::KeyUp, key::kEnter));
    check(!demo.child_2_.state_pressed_, "releasing the key releases it");

    // Space activates on key up, like a native button.
    router.dispatch(fixture.root, keyboard(ui::InputEventType::KeyDown, key::kSpace));
    router.dispatch(fixture.root, keyboard(ui::InputEventType::KeyUp, key::kSpace));
    check(demo.state_count_ == 2, "Space activates the enabled Button");

    // Disabled: neither key nor pointer reaches onPress, and it never shows as pressed.
    router.focus_manager().request_focus(&disabled);
    router.dispatch(fixture.root, keyboard(ui::InputEventType::KeyDown, key::kEnter));
    router.dispatch(fixture.root, keyboard(ui::InputEventType::KeyUp, key::kEnter));
    router.dispatch(fixture.root, pointer(ui::InputEventType::PointerDown, 20, 60));
    router.dispatch(fixture.root, pointer(ui::InputEventType::PointerUp, 20, 60));
    check(demo.state_count_ == 2, "a disabled Button never calls onPress");
    check(!demo.child_4_.state_pressed_, "a disabled Button is never pressed");

    // Pointer: press and release on the small danger Button.
    router.dispatch(fixture.root, pointer(ui::InputEventType::PointerDown, 20, 100));
    // The label is the hit target and only Click bubbles through generated bindings, so a pointer
    // press styles the Button through `:active` (set on the target chain), not the `pressed` class.
    check(ui::style::has_state(danger.pseudo_state(), ui::style::PseudoState::Active),
          "pointer down makes the danger Button :active");
    router.dispatch(fixture.root, pointer(ui::InputEventType::PointerUp, 20, 100));
    check(demo.state_count_ == 12, "a pointer click calls onPress (+10)");
    check(!ui::style::has_state(danger.pseudo_state(), ui::style::PseudoState::Active),
          "pointer up clears :active");

    // Pointer leaving cancels the press without activating.
    router.dispatch(fixture.root, pointer(ui::InputEventType::PointerDown, 20, 20));
    router.dispatch(fixture.root, pointer(ui::InputEventType::PointerMove, 300, 150));
    router.dispatch(fixture.root, pointer(ui::InputEventType::PointerUp, 300, 150));
    check(demo.state_count_ == 12, "leaving before release does not activate");
    check(!ui::style::has_state(enabled.pseudo_state(), ui::style::PseudoState::Active),
          "leaving clears :active");

    // Accessibility contract on the nodes.
    check(enabled.role() == "button" && enabled.focusable(), "the Button is a focusable button");
    check(has_class(enabled, "tbtn") && has_class(enabled, "tbtn-primary") &&
              has_class(enabled, "tbtn-md"),
          "the enabled Button carries its variant and size classes: " +
              std::string(enabled.class_name()));
    check(has_class(disabled, "tbtn-disabled"), "the disabled Button carries tbtn-disabled");
    check(has_class(danger, "tbtn-danger") && has_class(danger, "tbtn-sm"),
          "the danger Button carries its variant and size classes: " +
              std::string(danger.class_name()));

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "library_button_runtime: ok\n";
    return 0;
}
