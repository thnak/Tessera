// SPDX-License-Identifier: MIT
//
// `press_feedback` (ADR-064 item 7, spike #658): the thinnest complete behavior, driven headlessly
// through the scene's normalized input dispatch.
//
// The behavior owns no listeners: `InputDispatch` already normalizes pointer, touch and keyboard
// input into `hovered()`/`active()`, and `FocusManager` owns keyboard focus. These tests therefore
// prove the attach/update/detach seam and the output contract, not a new event mechanism.

#include "tessera/ui/behavior/press_feedback.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <string>

namespace {

using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::behavior::ArgsView;
using tessera::ui::behavior::ArgValue;
using tessera::ui::behavior::OutputValue;
using tessera::ui::behavior::PressFeedback;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "press_feedback: " << message << "\n";
    }
    return condition;
}

InputEvent pointer(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    return event;
}

// A scene with one 40x40 button at the origin, so a pointer at (20, 20) hits it.
struct Scene {
    Node root{1};
    Node* button = nullptr;
    InputRouter router;

    Scene() {
        root.set_bounds({0, 0, 200, 200});
        auto owner = std::make_unique<Node>(2);
        owner->set_bounds({0, 0, 40, 40});
        owner->set_focusable(true);
        button = &root.add_child(std::move(owner));
    }

    void move(float x, float y) {
        router.dispatch(root, pointer(InputEventType::PointerMove, x, y));
    }
    void down(float x, float y) {
        router.dispatch(root, pointer(InputEventType::PointerDown, x, y));
    }
    void up(float x, float y) {
        router.dispatch(root, pointer(InputEventType::PointerUp, x, y));
    }
};

// The three outputs in manifest order: `pressed`, `hovered`, `focused`.
struct Outputs {
    bool pressed = false;
    bool hovered = false;
    bool focused = false;
};

Outputs read(const PressFeedback& behavior) {
    const std::span<const OutputValue> values = behavior.outputs();
    Outputs out;
    if (values.size() >= 3) {
        out.pressed = values[0].boolean;
        out.hovered = values[1].boolean;
        out.focused = values[2].boolean;
    }
    return out;
}

// Before attach every output holds its manifest default, which for all three is `false`.
bool test_defaults_before_attach() {
    PressFeedback behavior;
    const Outputs out = read(behavior);
    return expect(!out.pressed && !out.hovered && !out.focused,
                  "every output defaults to false before attach") &&
           expect(behavior.target() == nullptr, "an unattached behavior has no target");
}

// A pointer press on a hovered button reports both `pressed` and `hovered`; the two are
// independent.
bool test_pointer_press_reports_pressed_and_hovered() {
    Scene scene;
    PressFeedback behavior;
    std::array<Node*, 1> nodes{scene.button};
    behavior.attach(Targets{nodes}, Services{&scene.router}, ArgsView{});

    bool ok = expect(behavior.target() == scene.button, "attach records the target");
    scene.move(20, 20);
    behavior.update(ArgsView{});
    Outputs out = read(behavior);
    ok = expect(out.hovered && !out.pressed, "a hover reports hovered only") && ok;

    scene.down(20, 20);
    behavior.update(ArgsView{});
    out = read(behavior);
    ok = expect(out.pressed && out.hovered, "a press on a hovered button reports both") && ok;

    scene.up(20, 20);
    behavior.update(ArgsView{});
    out = read(behavior);
    ok = expect(!out.pressed && out.hovered, "releasing clears pressed but keeps hovered") && ok;
    return ok;
}

// A disabled target never reports `pressed`, but still reports `hovered` and `focused`: a disabled
// control that is focusable must still show where focus is (ADR-064 item 9).
bool test_disabled_suppresses_pressed_only() {
    Scene scene;
    PressFeedback behavior;
    std::array<Node*, 1> nodes{scene.button};
    ArgValue disabled;
    disabled.kind = ArgValue::Kind::Bool;
    disabled.boolean = true;
    std::array<ArgValue, 1> args{disabled};
    behavior.attach(Targets{nodes}, Services{&scene.router}, ArgsView{args});

    scene.move(20, 20);
    scene.down(20, 20);
    behavior.update(ArgsView{args});
    Outputs out = read(behavior);
    bool ok = expect(!out.pressed, "a disabled target never reports pressed");
    ok = expect(out.hovered, "a disabled target still reports hovered") && ok;

    scene.router.focus_manager().request_focus(scene.button);
    behavior.update(ArgsView{args});
    out = read(behavior);
    ok = expect(out.focused, "a disabled target still reports focused") && ok;
    ok = expect(!out.pressed, "focus does not make a disabled target pressed") && ok;

    // Clearing the option lets the press through again.
    ArgValue enabled;
    enabled.kind = ArgValue::Kind::Bool;
    enabled.boolean = false;
    std::array<ArgValue, 1> on{enabled};
    behavior.update(ArgsView{on});
    ok = expect(read(behavior).pressed, "clearing disabled lets the press through") && ok;
    return ok;
}

// Moving the pointer off the button clears `hovered` without touching `pressed`.
bool test_pointer_leave_clears_hovered() {
    Scene scene;
    PressFeedback behavior;
    std::array<Node*, 1> nodes{scene.button};
    behavior.attach(Targets{nodes}, Services{&scene.router}, ArgsView{});

    scene.move(20, 20);
    behavior.update(ArgsView{});
    bool ok = expect(read(behavior).hovered, "the button is hovered");

    scene.move(150, 150);
    behavior.update(ArgsView{});
    ok = expect(!read(behavior).hovered, "leaving the button clears hovered") && ok;
    return ok;
}

// Keyboard focus is a separate output: focusing the button reports `focused` without `hovered`.
bool test_keyboard_focus_is_independent() {
    Scene scene;
    PressFeedback behavior;
    std::array<Node*, 1> nodes{scene.button};
    behavior.attach(Targets{nodes}, Services{&scene.router}, ArgsView{});

    scene.router.focus_manager().request_focus(scene.button);
    behavior.update(ArgsView{});
    const Outputs out = read(behavior);
    return expect(out.focused, "focusing the button reports focused") &&
           expect(!out.hovered, "keyboard focus does not imply hover") &&
           expect(!out.pressed, "keyboard focus does not imply pressed");
}

// Detach is idempotent and returns every output to its default, so a component that unmounts never
// reads a stale press.
bool test_detach_is_idempotent_and_resets() {
    Scene scene;
    PressFeedback behavior;
    std::array<Node*, 1> nodes{scene.button};
    behavior.attach(Targets{nodes}, Services{&scene.router}, ArgsView{});
    scene.move(20, 20);
    scene.down(20, 20);
    behavior.update(ArgsView{});
    bool ok = expect(read(behavior).pressed, "the button is pressed before detach");

    behavior.detach();
    Outputs out = read(behavior);
    ok = expect(!out.pressed && !out.hovered && !out.focused, "detach resets every output") && ok;
    ok = expect(behavior.target() == nullptr, "detach clears the target") && ok;

    behavior.detach();
    out = read(behavior);
    ok = expect(!out.pressed && !out.hovered && !out.focused, "a second detach is a no-op") && ok;
    return ok;
}

// A behavior whose target is not attached does nothing: the conditional-target rule of the spec
// (section 8, rule 2) says a behavior is never half-attached.
bool test_unattached_target_is_a_no_op() {
    Scene scene;
    PressFeedback behavior;
    std::array<Node*, 1> nodes{nullptr};
    behavior.attach(Targets{nodes}, Services{&scene.router}, ArgsView{});
    const Outputs out = read(behavior);
    return expect(behavior.target() == nullptr, "a null target does not attach") &&
           expect(!out.pressed && !out.hovered && !out.focused,
                  "a null target leaves every output at its default");
}

// A build with no scene hands in a null dispatch; the behavior must treat that as "nothing is
// pressed" rather than dereferencing it.
bool test_null_dispatch_is_a_no_op() {
    Scene scene;
    PressFeedback behavior;
    std::array<Node*, 1> nodes{scene.button};
    behavior.attach(Targets{nodes}, Services{nullptr}, ArgsView{});
    behavior.update(ArgsView{});
    const Outputs out = read(behavior);
    return expect(!out.pressed && !out.hovered && !out.focused,
                  "a null dispatch leaves every output at its default");
}

// The options are empty, so an update with no arguments must not disturb the outputs.
bool test_update_with_no_options_keeps_the_state() {
    Scene scene;
    PressFeedback behavior;
    std::array<Node*, 1> nodes{scene.button};
    behavior.attach(Targets{nodes}, Services{&scene.router}, ArgsView{});
    scene.move(20, 20);
    behavior.update(ArgsView{});
    bool ok = expect(read(behavior).hovered, "the button is hovered");
    behavior.update(ArgsView{});
    ok = expect(read(behavior).hovered, "a second update keeps the state") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = test_defaults_before_attach();
    ok = test_pointer_press_reports_pressed_and_hovered() && ok;
    ok = test_disabled_suppresses_pressed_only() && ok;
    ok = test_pointer_leave_clears_hovered() && ok;
    ok = test_keyboard_focus_is_independent() && ok;
    ok = test_detach_is_idempotent_and_resets() && ok;
    ok = test_unattached_target_is_a_no_op() && ok;
    ok = test_null_dispatch_is_a_no_op() && ok;
    ok = test_update_with_no_options_keeps_the_state() && ok;
    if (!ok) {
        std::cerr << "press_feedback: FAILED\n";
        return 1;
    }
    std::cout << "press_feedback: ok\n";
    return 0;
}
