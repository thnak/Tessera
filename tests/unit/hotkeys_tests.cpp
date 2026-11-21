// SPDX-License-Identifier: MIT
//
// `hotkeys` (ADR-064 item 7, issue #627): declare keyboard shortcuts for a scope.
//
// The engine owns the mechanism (`KeyBindingRegistry` plus the router's dispatch order); this
// behavior owns the hook's half: parsing the chord list, registering it into the window's one
// registry, and removing exactly what it registered. These tests drive the real registry through
// the real router, so a rule about a chord firing is proved by dispatching a key event.

#include "tessera/ui/behavior/hotkeys.hpp"
#include "tessera/ui/input/key_bindings.hpp"
#include "tessera/ui/key_code.hpp"
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
using tessera::ui::Modifier;
using tessera::ui::Node;
using tessera::ui::behavior::ArgsView;
using tessera::ui::behavior::ArgValue;
using tessera::ui::behavior::Hotkeys;
using tessera::ui::behavior::OutputValue;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "hotkeys: " << message << "\n";
        ++failures;
    }
}

ArgValue string_arg(std::string_view value) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::String;
    arg.text = value;
    return arg;
}

ArgValue bool_arg(bool value) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::Bool;
    arg.boolean = value;
    return arg;
}

// The manifest's option order: `chords`, `allowInTextInput`, `repeat`.
std::array<ArgValue, 3> options(std::string_view chords, bool allow_in_text = false,
                                bool repeat = false) {
    return {string_arg(chords), bool_arg(allow_in_text), bool_arg(repeat)};
}

InputEvent key_down(tessera::ui::KeyCode key, Modifier modifiers = Modifier::None) {
    InputEvent event;
    event.type = InputEventType::KeyDown;
    event.key = key;
    event.modifiers = modifiers;
    return event;
}

// A scene with one focusable node, so a key event has a target to dispatch to.
struct Scene {
    Node root{1};
    Node* button = nullptr;
    InputRouter router;

    Scene() {
        root.set_bounds({0, 0, 400, 400});
        auto node = std::make_unique<Node>(2);
        node->set_bounds({0, 0, 40, 40});
        node->set_focusable(true);
        button = &root.add_child(std::move(node));
        router.focus_manager().set_root(root);
        router.focus_manager().request_focus(button);
    }
};

void test_parse_chord() {
    Hotkeys::Chord chord;
    expect(Hotkeys::parse_chord("ctrl+s", chord), "ctrl+s parses");
    expect(chord.key == 'S', "the key is the upper-case ASCII code");
    expect(chord.modifiers == static_cast<std::uint32_t>(Modifier::Control),
           "the modifier is Control");

    expect(Hotkeys::parse_chord("shift+alt+f1", chord), "shift+alt+f1 parses");
    expect(chord.key == tessera::ui::key::kF1, "F1 is the named key");
    expect(chord.modifiers == (static_cast<std::uint32_t>(Modifier::Shift) |
                               static_cast<std::uint32_t>(Modifier::Alt)),
           "both modifiers are set");

    expect(Hotkeys::parse_chord("escape", chord), "a bare named key parses");
    expect(chord.key == tessera::ui::key::kEscape, "escape is the named key");
    expect(chord.modifiers == 0, "a bare key has no modifiers");

    expect(Hotkeys::parse_chord("meta+k", chord), "meta+k parses");
    expect(chord.modifiers == static_cast<std::uint32_t>(Modifier::Meta), "meta is Meta");

    expect(!Hotkeys::parse_chord("hyper+s", chord), "an unknown modifier is rejected");
    expect(!Hotkeys::parse_chord("ctrl+", chord), "a trailing plus is rejected");
    expect(!Hotkeys::parse_chord("", chord), "an empty chord is rejected");
    expect(!Hotkeys::parse_chord("ctrl+nosuchkey", chord), "an unknown key is rejected");
}

void test_attach_registers() {
    Scene scene;
    Hotkeys hotkeys;
    auto args = options("ctrl+s,escape");
    hotkeys.attach(Targets{}, Services{&scene.router}, ArgsView{args});
    expect(hotkeys.registered() == 2, "two chords are registered");
    expect(scene.router.accelerators().size() == 2, "the window registry holds both");
    const auto outputs = hotkeys.outputs();
    expect(outputs.size() == 1 && outputs[0].kind == OutputValue::Kind::Bool && outputs[0].boolean,
           "the `registered` output reports the bindings are live");
}

void test_chord_fires() {
    Scene scene;
    Hotkeys hotkeys;
    auto args = options("ctrl+s");
    hotkeys.attach(Targets{}, Services{&scene.router}, ArgsView{args});
    const auto result = scene.router.dispatch(scene.root, key_down('S', Modifier::Control));
    expect(result.accelerator_fired != tessera::ui::kInvalidBindingId,
           "the registered chord fires through the router");
}

void test_unregistered_chord_does_not_fire() {
    Scene scene;
    Hotkeys hotkeys;
    auto args = options("ctrl+s");
    hotkeys.attach(Targets{}, Services{&scene.router}, ArgsView{args});
    const auto result = scene.router.dispatch(scene.root, key_down('S', Modifier::Shift));
    expect(result.accelerator_fired == tessera::ui::kInvalidBindingId,
           "a different modifier set does not fire the binding");
}

void test_detach_removes() {
    Scene scene;
    Hotkeys hotkeys;
    auto args = options("ctrl+s,escape");
    hotkeys.attach(Targets{}, Services{&scene.router}, ArgsView{args});
    hotkeys.detach();
    expect(hotkeys.registered() == 0, "detach removes every registration");
    expect(scene.router.accelerators().size() == 0, "the window registry is empty again");
    expect(!hotkeys.outputs()[0].boolean, "a detached behavior reports the manifest default");
    const auto result = scene.router.dispatch(scene.root, key_down('S', Modifier::Control));
    expect(result.accelerator_fired == tessera::ui::kInvalidBindingId,
           "a detached chord no longer fires");
}

void test_update_replaces() {
    Scene scene;
    Hotkeys hotkeys;
    auto first = options("ctrl+s");
    hotkeys.attach(Targets{}, Services{&scene.router}, ArgsView{first});
    expect(scene.router.accelerators().size() == 1, "one chord is registered");

    auto second = options("ctrl+k,ctrl+j");
    hotkeys.update(ArgsView{second});
    expect(hotkeys.registered() == 2, "the update registers the new set");
    expect(scene.router.accelerators().size() == 2,
           "the old chord is removed, so the registry holds only the new set");
    const auto old = scene.router.dispatch(scene.root, key_down('S', Modifier::Control));
    expect(old.accelerator_fired == tessera::ui::kInvalidBindingId,
           "the old chord no longer fires");
    const auto fresh = scene.router.dispatch(scene.root, key_down('K', Modifier::Control));
    expect(fresh.accelerator_fired != tessera::ui::kInvalidBindingId, "the new chord fires");
}

void test_malformed_chord_is_skipped() {
    Scene scene;
    Hotkeys hotkeys;
    auto args = options("ctrl+s,hyper+k,escape");
    hotkeys.attach(Targets{}, Services{&scene.router}, ArgsView{args});
    expect(hotkeys.registered() == 2, "the malformed chord is skipped, the others register");
}

void test_no_router_registers_nothing() {
    Hotkeys hotkeys;
    auto args = options("ctrl+s");
    hotkeys.attach(Targets{}, Services{}, ArgsView{args});
    expect(hotkeys.registered() == 0, "a build with no router registers nothing");
    expect(!hotkeys.outputs()[0].boolean, "and reports no bindings");
}

// A chord that fires while a text field has focus steals a key the user is typing, so the default
// is to withdraw it; `allowInTextInput` opts in.
void test_text_input_suppresses_by_default() {
    Scene scene;
    auto field = std::make_unique<Node>(3);
    field->set_bounds({0, 100, 200, 40});
    field->set_focusable(true);
    field->set_element_kind("TextInput");
    Node* input = &scene.root.add_child(std::move(field));
    scene.router.focus_manager().request_focus(input);

    Hotkeys hotkeys;
    auto args = options("ctrl+s");
    hotkeys.attach(Targets{}, Services{&scene.router}, ArgsView{args});
    expect(hotkeys.registered() == 0, "a chord is withdrawn while a text field has focus");
    expect(!hotkeys.outputs()[0].boolean, "and reports no bindings");

    auto allowed = options("ctrl+s", true);
    hotkeys.update(ArgsView{allowed});
    expect(hotkeys.registered() == 1, "allowInTextInput registers the chord anyway");
    const auto result = scene.router.dispatch(scene.root, key_down('S', Modifier::Control));
    expect(result.accelerator_fired != tessera::ui::kInvalidBindingId,
           "the opted-in chord fires while the field has focus");
}

// A behavior destroyed without an explicit detach must not leave a chord registered.
void test_destructor_removes() {
    Scene scene;
    {
        Hotkeys hotkeys;
        auto args = options("ctrl+s");
        hotkeys.attach(Targets{}, Services{&scene.router}, ArgsView{args});
        expect(scene.router.accelerators().size() == 1, "the chord is registered inside the scope");
    }
    expect(scene.router.accelerators().size() == 0, "the destructor removes the chord");
}

} // namespace

int main() {
    test_parse_chord();
    test_attach_registers();
    test_chord_fires();
    test_unregistered_chord_does_not_fire();
    test_detach_removes();
    test_update_replaces();
    test_malformed_chord_is_skipped();
    test_no_router_registers_nothing();
    test_text_input_suppresses_by_default();
    test_destructor_removes();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
