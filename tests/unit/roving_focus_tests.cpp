// SPDX-License-Identifier: MIT
//
// `roving_focus` (ADR-064 item 7, issue #625): one Tab stop for a group of items, with the arrow
// keys moving focus among them.
//
// The engine owns the mechanism (`Node::set_focus_group` plus `FocusManager::move_in_focus_group`);
// this behavior owns the hook's half: the axis from `orientation`, the `typeahead` flag, and the
// `activeIndex` output. These tests drive the real `FocusManager`, so a rule about arrow keys is
// proved by pressing arrow keys.

#include "tessera/ui/behavior/roving_focus.hpp"
#include "tessera/ui/key_code.hpp"
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
using tessera::ui::behavior::OutputValue;
using tessera::ui::behavior::RovingFocus;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "roving_focus: " << message << "\n";
        ++failures;
    }
}

// root -> list(2) -> { one(3), two(4), three(5) }. Every item is focusable, so the group has three
// candidates and the root is the one a group must not include.
struct Scene {
    Node root{1};
    Node* outside = nullptr;
    Node* list = nullptr;
    Node* one = nullptr;
    Node* two = nullptr;
    Node* three = nullptr;
    InputRouter router;

    Scene() {
        root.set_bounds({0, 0, 400, 400});
        auto out = std::make_unique<Node>(6);
        out->set_bounds({0, 350, 200, 40});
        out->set_focusable(true);
        outside = &root.add_child(std::move(out));
        auto box = std::make_unique<Node>(2);
        box->set_bounds({0, 0, 200, 300});
        list = &root.add_child(std::move(box));
        one = add_item(3, 0);
        two = add_item(4, 50);
        three = add_item(5, 100);
        router.focus_manager().set_root(root);
    }

    Node* add_item(tessera::ui::NodeId id, float y) {
        auto item = std::make_unique<Node>(id);
        item->set_bounds({0, y, 200, 40});
        item->set_focusable(true);
        return &list->add_child(std::move(item));
    }
};

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

// The manifest's option order: `orientation`, `wrap`, `typeahead`.
std::array<ArgValue, 3> options(std::string_view orientation, bool wrap = true,
                                bool typeahead = false) {
    return {string_arg(orientation), bool_arg(wrap), bool_arg(typeahead)};
}

void test_attach_sets_the_group() {
    Scene scene;
    RovingFocus focus;
    auto args = options("vertical");
    focus.attach(Targets{std::array<Node*, 1>{scene.list}}, Services{&scene.router},
                 ArgsView{args});
    expect(scene.list->focus_group() == Node::FocusGroup::Vertical,
           "a vertical group marks its target");
    expect(focus.target() == scene.list, "the behavior reports its target");
    const auto outputs = focus.outputs();
    expect(outputs.size() == 1 && outputs[0].kind == OutputValue::Kind::Number,
           "the `activeIndex` output is a number");
    expect(outputs[0].number == -1.0, "no item holds the Tab stop before focus enters the group");
}

void test_orientation_selects_the_axis() {
    Scene scene;
    RovingFocus focus;
    auto horizontal = options("horizontal");
    focus.attach(Targets{std::array<Node*, 1>{scene.list}}, Services{&scene.router},
                 ArgsView{horizontal});
    expect(scene.list->focus_group() == Node::FocusGroup::Horizontal,
           "a horizontal group marks its target");
    auto both = options("both");
    focus.update(ArgsView{both});
    expect(scene.list->focus_group() == Node::FocusGroup::Both, "an update changes the axis");
}

void test_typeahead_flag() {
    Scene scene;
    RovingFocus focus;
    auto off = options("vertical", true, false);
    focus.attach(Targets{std::array<Node*, 1>{scene.list}}, Services{&scene.router}, ArgsView{off});
    expect(!scene.list->typeahead(), "typeahead is off by default");
    auto on = options("vertical", true, true);
    focus.update(ArgsView{on});
    expect(scene.list->typeahead(), "an update turns typeahead on");
}

void test_detach_clears_the_group() {
    Scene scene;
    RovingFocus focus;
    auto args = options("vertical", true, true);
    focus.attach(Targets{std::array<Node*, 1>{scene.list}}, Services{&scene.router},
                 ArgsView{args});
    focus.detach();
    expect(scene.list->focus_group() == Node::FocusGroup::None,
           "detach clears the group it set, so no group outlives its component");
    expect(!scene.list->typeahead(), "detach clears the typeahead flag too");
    expect(focus.target() == nullptr, "a detached behavior has no target");
    expect(focus.outputs()[0].number == 0.0, "a detached behavior reports the manifest default");
}

void test_missing_target_attaches_nothing() {
    Scene scene;
    RovingFocus focus;
    auto args = options("vertical");
    focus.attach(Targets{std::array<Node*, 1>{nullptr}}, Services{&scene.router}, ArgsView{args});
    expect(focus.target() == nullptr, "a behavior with an unresolved target attaches nothing");
    expect(scene.list->focus_group() == Node::FocusGroup::None, "and marks no node");
}

// The rule the hook exists for: the arrow keys move focus among the items and wrap at both ends.
void test_arrow_keys_move_and_wrap() {
    Scene scene;
    RovingFocus focus;
    auto args = options("vertical");
    focus.attach(Targets{std::array<Node*, 1>{scene.list}}, Services{&scene.router},
                 ArgsView{args});
    scene.router.focus_manager().request_focus(scene.one);
    expect(scene.router.focus_manager().focused() == scene.one, "focus starts on the first item");

    expect(
        scene.router.focus_manager().move_in_focus_group(scene.root, tessera::ui::key::kArrowDown),
        "ArrowDown moves within the group");
    expect(scene.router.focus_manager().focused() == scene.two, "ArrowDown moves to the next item");
    scene.router.focus_manager().move_in_focus_group(scene.root, tessera::ui::key::kArrowDown);
    expect(scene.router.focus_manager().focused() == scene.three,
           "ArrowDown moves to the last item");
    scene.router.focus_manager().move_in_focus_group(scene.root, tessera::ui::key::kArrowDown);
    expect(scene.router.focus_manager().focused() == scene.one,
           "ArrowDown wraps to the first item instead of leaving the group");
    scene.router.focus_manager().move_in_focus_group(scene.root, tessera::ui::key::kArrowUp);
    expect(scene.router.focus_manager().focused() == scene.three, "ArrowUp wraps the other way");
}

// Home and End jump to the ends of the group.
void test_home_and_end() {
    Scene scene;
    RovingFocus focus;
    auto args = options("vertical");
    focus.attach(Targets{std::array<Node*, 1>{scene.list}}, Services{&scene.router},
                 ArgsView{args});
    scene.router.focus_manager().request_focus(scene.two);
    scene.router.focus_manager().move_in_focus_group(scene.root, tessera::ui::key::kEnd);
    expect(scene.router.focus_manager().focused() == scene.three, "End moves to the last item");
    scene.router.focus_manager().move_in_focus_group(scene.root, tessera::ui::key::kHome);
    expect(scene.router.focus_manager().focused() == scene.one, "Home moves to the first item");
}

// A horizontal group ignores the vertical arrows, which is what makes the axis meaningful.
void test_axis_is_respected() {
    Scene scene;
    RovingFocus focus;
    auto args = options("horizontal");
    focus.attach(Targets{std::array<Node*, 1>{scene.list}}, Services{&scene.router},
                 ArgsView{args});
    scene.router.focus_manager().request_focus(scene.one);
    expect(
        !scene.router.focus_manager().move_in_focus_group(scene.root, tessera::ui::key::kArrowDown),
        "a horizontal group ignores ArrowDown");
    expect(scene.router.focus_manager().focused() == scene.one, "focus did not move");
    expect(
        scene.router.focus_manager().move_in_focus_group(scene.root, tessera::ui::key::kArrowRight),
        "a horizontal group honours ArrowRight");
    expect(scene.router.focus_manager().focused() == scene.two,
           "ArrowRight moves to the next item");
}

// The output reports the index of the item that holds the Tab stop, which is what a component needs
// to render a highlight.
void test_active_index_tracks_focus() {
    Scene scene;
    RovingFocus focus;
    auto args = options("vertical");
    focus.attach(Targets{std::array<Node*, 1>{scene.list}}, Services{&scene.router},
                 ArgsView{args});
    scene.router.focus_manager().request_focus(scene.two);
    focus.update(ArgsView{args});
    expect(focus.outputs()[0].number == 1.0, "the second item reports index 1");
    scene.router.focus_manager().request_focus(scene.three);
    focus.update(ArgsView{args});
    expect(focus.outputs()[0].number == 2.0, "the third item reports index 2");
    // Focus outside the group is not an index of it.
    scene.router.focus_manager().request_focus(scene.outside);
    focus.update(ArgsView{args});
    expect(focus.outputs()[0].number == -1.0, "focus outside the group reports -1");
}

} // namespace

int main() {
    test_attach_sets_the_group();
    test_orientation_selects_the_axis();
    test_typeahead_flag();
    test_detach_clears_the_group();
    test_missing_target_attaches_nothing();
    test_arrow_keys_move_and_wrap();
    test_home_and_end();
    test_axis_is_respected();
    test_active_index_tracks_focus();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
