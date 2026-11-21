// SPDX-License-Identifier: MIT
//
// `interactive_role` (ADR-064 item 7, issue #635): tell a child it sits inside an interactive
// parent.
//
// The defect it prevents is real: a clickable card containing a button produces a button inside a
// button, which a screen reader announces twice and a pointer press triggers twice. The flag lives
// on the node, because the descendant that needs the answer is a different component and the scene
// is the only thing they share.

#include "tessera/ui/behavior/interactive_role.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <string>

namespace {

using tessera::ui::Node;
using tessera::ui::behavior::ArgsView;
using tessera::ui::behavior::ArgValue;
using tessera::ui::behavior::InteractiveRole;
using tessera::ui::behavior::OutputValue;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "interactive_role: " << message << "\n";
        ++failures;
    }
}

ArgValue bool_arg(bool value) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::Bool;
    arg.boolean = value;
    return arg;
}

std::array<ArgValue, 1> options(bool interactive) {
    return {bool_arg(interactive)};
}

// root -> card(2) -> { label(3), button(4) }, and a sibling(5) outside the card.
struct Scene {
    Node root{1};
    Node* card = nullptr;
    Node* label = nullptr;
    Node* button = nullptr;
    Node* sibling = nullptr;

    Scene() {
        root.set_bounds({0, 0, 400, 400});
        auto box = std::make_unique<Node>(2);
        box->set_bounds({0, 0, 200, 200});
        card = &root.add_child(std::move(box));

        auto text = std::make_unique<Node>(3);
        text->set_bounds({0, 0, 200, 40});
        label = &card->add_child(std::move(text));

        auto control = std::make_unique<Node>(4);
        control->set_bounds({0, 50, 80, 40});
        control->set_focusable(true);
        button = &card->add_child(std::move(control));

        auto other = std::make_unique<Node>(5);
        other->set_bounds({0, 250, 200, 40});
        sibling = &root.add_child(std::move(other));
    }
};

void test_attach_marks_the_target() {
    Scene scene;
    InteractiveRole role;
    auto args = options(true);
    role.attach(Targets{std::array<Node*, 1>{scene.card}}, Services{}, ArgsView{args});
    expect(scene.card->interactive_parent(), "an interactive target is marked");
    expect(role.target() == scene.card, "the behavior reports its target");
    const auto outputs = role.outputs();
    expect(outputs.size() == 1 && outputs[0].kind == OutputValue::Kind::Bool,
           "the `nested` output is a boolean");
    expect(!outputs[0].boolean, "the interactive surface itself is not nested");
}

void test_descendant_sees_the_parent() {
    Scene scene;
    InteractiveRole role;
    auto args = options(true);
    role.attach(Targets{std::array<Node*, 1>{scene.card}}, Services{}, ArgsView{args});
    expect(InteractiveRole::has_interactive_ancestor(*scene.button),
           "a descendant of the interactive surface is nested");
    expect(InteractiveRole::has_interactive_ancestor(*scene.label),
           "every descendant is nested, not only controls");
    expect(!InteractiveRole::has_interactive_ancestor(*scene.sibling),
           "a node outside the surface is not nested");
    expect(!InteractiveRole::has_interactive_ancestor(*scene.card),
           "the surface does not count itself as an ancestor");
}

void test_inactive_marks_nothing() {
    Scene scene;
    InteractiveRole role;
    auto args = options(false);
    role.attach(Targets{std::array<Node*, 1>{scene.card}}, Services{}, ArgsView{args});
    expect(!scene.card->interactive_parent(), "an inactive target is not marked");
    expect(!InteractiveRole::has_interactive_ancestor(*scene.button),
           "and its descendants are not nested");
}

void test_update_toggles() {
    Scene scene;
    InteractiveRole role;
    auto on = options(true);
    role.attach(Targets{std::array<Node*, 1>{scene.card}}, Services{}, ArgsView{on});
    expect(InteractiveRole::has_interactive_ancestor(*scene.button), "the descendant is nested");

    auto off = options(false);
    role.update(ArgsView{off});
    expect(!scene.card->interactive_parent(), "an update clears the flag");
    expect(!InteractiveRole::has_interactive_ancestor(*scene.button),
           "and the descendant is no longer nested");
}

void test_detach_clears() {
    Scene scene;
    InteractiveRole role;
    auto args = options(true);
    role.attach(Targets{std::array<Node*, 1>{scene.card}}, Services{}, ArgsView{args});
    role.detach();
    expect(!scene.card->interactive_parent(),
           "detach clears the flag it set, so no surface outlives its component");
    expect(role.target() == nullptr, "a detached behavior has no target");
    expect(!role.outputs()[0].boolean, "a detached behavior reports the manifest default");
}

// A nested surface reports itself as nested, which is what a component needs to decide whether to
// render its own control.
void test_nested_surface_reports_nested() {
    Scene scene;
    InteractiveRole outer;
    InteractiveRole inner;
    auto args = options(true);
    outer.attach(Targets{std::array<Node*, 1>{scene.card}}, Services{}, ArgsView{args});
    // The button is itself an interactive surface inside the card.
    inner.attach(Targets{std::array<Node*, 1>{scene.button}}, Services{}, ArgsView{args});
    expect(scene.button->interactive_parent(), "the inner surface is marked");
    expect(inner.outputs()[0].boolean,
           "the inner surface reports itself nested inside the outer one");
    expect(!outer.outputs()[0].boolean, "the outer surface is not nested");
}

void test_missing_target_attaches_nothing() {
    Scene scene;
    InteractiveRole role;
    auto args = options(true);
    role.attach(Targets{std::array<Node*, 1>{nullptr}}, Services{}, ArgsView{args});
    expect(role.target() == nullptr, "a behavior with an unresolved target attaches nothing");
    expect(!scene.card->interactive_parent(), "and marks no node");
}

} // namespace

int main() {
    test_attach_marks_the_target();
    test_descendant_sees_the_parent();
    test_inactive_marks_nothing();
    test_update_toggles();
    test_detach_clears();
    test_nested_surface_reports_nested();
    test_missing_target_attaches_nothing();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
