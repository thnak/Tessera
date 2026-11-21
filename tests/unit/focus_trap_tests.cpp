// SPDX-License-Identifier: MIT
//
// `focus_trap` (ADR-064 item 7, issue #624): keep keyboard focus inside a container while it is
// active, and give it back when it is released.
//
// The engine owns the mechanism (`Node::set_focus_trap` plus `FocusManager::sync_focus_trap`); this
// behavior owns the hook's half: turning the flag on and off with the `active` option, and never
// leaving a trap behind for a component that no longer exists. These tests drive the real
// `FocusManager` headlessly, so a rule about Tab traversal is proved by Tab traversal.

#include "tessera/ui/behavior/focus_trap.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <string>

namespace {

using tessera::ui::FocusManager;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::behavior::ArgsView;
using tessera::ui::behavior::ArgValue;
using tessera::ui::behavior::FocusTrap;
using tessera::ui::behavior::OutputValue;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "focus_trap: " << message << "\n";
        ++failures;
    }
}

// root -> { outside(2), dialog(3) -> { first(4), second(5) } }. Every leaf is focusable, so a trap
// on the dialog has two candidates and the outside node is the one a trap must exclude.
struct Scene {
    Node root{1};
    Node* outside = nullptr;
    Node* dialog = nullptr;
    Node* first = nullptr;
    Node* second = nullptr;
    InputRouter router;

    Scene() {
        root.set_bounds({0, 0, 400, 400});
        auto out = std::make_unique<Node>(2);
        out->set_bounds({0, 0, 40, 40});
        out->set_focusable(true);
        outside = &root.add_child(std::move(out));

        auto box = std::make_unique<Node>(3);
        box->set_bounds({0, 50, 200, 200});
        dialog = &root.add_child(std::move(box));

        auto one = std::make_unique<Node>(4);
        one->set_bounds({0, 50, 40, 40});
        one->set_focusable(true);
        first = &dialog->add_child(std::move(one));

        auto two = std::make_unique<Node>(5);
        two->set_bounds({0, 100, 40, 40});
        two->set_focusable(true);
        second = &dialog->add_child(std::move(two));

        router.focus_manager().set_root(root);
    }
};

ArgValue bool_arg(bool value) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::Bool;
    arg.boolean = value;
    return arg;
}

// The manifest's option order: `active`, `restoreFocus`.
std::array<ArgValue, 2> options(bool active, bool restore_focus = true) {
    return {bool_arg(active), bool_arg(restore_focus)};
}

void test_attach_sets_the_trap() {
    Scene scene;
    FocusTrap trap;
    auto args = options(true);
    trap.attach(Targets{std::array<Node*, 1>{scene.dialog}}, Services{&scene.router},
                ArgsView{args});
    expect(scene.dialog->focus_trap(), "an active trap marks its target");
    expect(trap.target() == scene.dialog, "the behavior reports its target");
    const auto outputs = trap.outputs();
    expect(outputs.size() == 1 && outputs[0].kind == OutputValue::Kind::Bool && outputs[0].boolean,
           "the `active` output reports the trap is on");
}

void test_inactive_trap_marks_nothing() {
    Scene scene;
    FocusTrap trap;
    auto args = options(false);
    trap.attach(Targets{std::array<Node*, 1>{scene.dialog}}, Services{&scene.router},
                ArgsView{args});
    expect(!scene.dialog->focus_trap(), "an inactive trap leaves the target untrapped");
    const auto outputs = trap.outputs();
    expect(outputs.size() == 1 && !outputs[0].boolean,
           "the `active` output reports the trap is off");
}

void test_update_toggles_the_trap() {
    Scene scene;
    FocusTrap trap;
    auto on = options(true);
    trap.attach(Targets{std::array<Node*, 1>{scene.dialog}}, Services{&scene.router}, ArgsView{on});
    expect(scene.dialog->focus_trap(), "the trap starts on");

    auto off = options(false);
    trap.update(ArgsView{off});
    expect(!scene.dialog->focus_trap(), "an update turns the trap off");
    expect(!trap.outputs()[0].boolean, "the output follows the option");

    trap.update(ArgsView{on});
    expect(scene.dialog->focus_trap(), "an update turns the trap back on");
}

void test_detach_clears_the_trap() {
    Scene scene;
    FocusTrap trap;
    auto args = options(true);
    trap.attach(Targets{std::array<Node*, 1>{scene.dialog}}, Services{&scene.router},
                ArgsView{args});
    expect(scene.dialog->focus_trap(), "the trap is on before detach");
    trap.detach();
    expect(!scene.dialog->focus_trap(),
           "detach clears the flag it set, so no trap outlives its component");
    expect(trap.target() == nullptr, "a detached behavior has no target");
    expect(!trap.outputs()[0].boolean, "a detached behavior reports the manifest default");
}

// Detach is idempotent, and a second detach must not touch a flag the behavior no longer owns.
void test_detach_is_idempotent() {
    Scene scene;
    FocusTrap trap;
    auto args = options(true);
    trap.attach(Targets{std::array<Node*, 1>{scene.dialog}}, Services{&scene.router},
                ArgsView{args});
    trap.detach();
    expect(!scene.dialog->focus_trap(), "the first detach clears the trap");
    // Another owner traps the node after this behavior released it.
    scene.dialog->set_focus_trap(true);
    trap.detach();
    expect(scene.dialog->focus_trap(),
           "a second detach does not clear a trap this behavior no longer owns");
}

void test_missing_target_attaches_nothing() {
    Scene scene;
    FocusTrap trap;
    auto args = options(true);
    trap.attach(Targets{std::array<Node*, 1>{nullptr}}, Services{&scene.router}, ArgsView{args});
    expect(trap.target() == nullptr, "a behavior with an unresolved target attaches nothing");
    expect(!scene.dialog->focus_trap(), "and marks no node");
}

// The rule the hook exists for: with the trap active, Tab traversal stays inside the dialog.
void test_tab_traversal_stays_inside() {
    Scene scene;
    FocusTrap trap;
    auto args = options(true);
    trap.attach(Targets{std::array<Node*, 1>{scene.dialog}}, Services{&scene.router},
                ArgsView{args});
    scene.router.focus_manager().sync_focus_trap(scene.root);
    expect(scene.router.focus_manager().modal_root() == scene.dialog,
           "the host's reconcile makes the dialog the modal root");

    scene.router.focus_manager().request_focus(scene.first);
    expect(scene.router.focus_manager().focused() == scene.first, "focus starts on the first item");
    scene.router.focus_manager().focus_next(scene.root);
    expect(scene.router.focus_manager().focused() == scene.second,
           "Tab moves to the next item inside the trap");
    scene.router.focus_manager().focus_next(scene.root);
    expect(scene.router.focus_manager().focused() == scene.first,
           "Tab wraps inside the trap instead of leaving it");
    expect(!scene.router.focus_manager().allows_focus(*scene.outside),
           "a node outside the trap is not focusable while it is active");
}

// Releasing the trap gives focus back, which is the `restoreFocus` half of the contract.
void test_release_restores_focus() {
    Scene scene;
    FocusTrap trap;
    scene.router.focus_manager().request_focus(scene.outside);
    expect(scene.router.focus_manager().focused() == scene.outside, "focus starts outside");

    auto on = options(true);
    trap.attach(Targets{std::array<Node*, 1>{scene.dialog}}, Services{&scene.router}, ArgsView{on});
    scene.router.focus_manager().sync_focus_trap(scene.root);
    expect(scene.router.focus_manager().focused() == scene.first,
           "activating the trap moves focus into the dialog");

    trap.detach();
    scene.router.focus_manager().sync_focus_trap(scene.root);
    expect(scene.router.focus_manager().focused() == scene.outside,
           "releasing the trap restores the focus held before it");
}

} // namespace

int main() {
    test_attach_sets_the_trap();
    test_inactive_trap_marks_nothing();
    test_update_toggles_the_trap();
    test_detach_clears_the_trap();
    test_detach_is_idempotent();
    test_missing_target_attaches_nothing();
    test_tab_traversal_stays_inside();
    test_release_restores_focus();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
