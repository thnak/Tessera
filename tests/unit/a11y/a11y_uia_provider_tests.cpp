// SPDX-License-Identifier: MIT
//
// Provider projection: the snapshot node -> UIA element mapping (control type, patterns, bounded
// text, bounds, state), parent/child navigation, focus/hit-test, and action validation. Runs on
// every platform; the Windows COM adapter consumes exactly this surface.

#include "a11y_uia_test_support.hpp"
#include "tessera/ui/accessibility/uia_provider.hpp"

#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace a11y_uia_test;
using tessera::ui::a11y::A11yIndex;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::has_pattern;
using tessera::ui::a11y::name_of;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::State;
using tessera::ui::a11y::text_of;
using tessera::ui::a11y::UiaActionRequest;
using tessera::ui::a11y::UiaControlType;
using tessera::ui::a11y::UiaElementInfo;
using tessera::ui::a11y::UiaError;
using tessera::ui::a11y::UiaPatterns;
using tessera::ui::a11y::UiaProvider;
using tessera::ui::a11y::value_of;

A11yNode element(Role role, std::string_view text, float x, float y, float width, float height) {
    A11yNode node = make_node(role, text);
    node.bounds.x = x;
    node.bounds.y = y;
    node.bounds.width = width;
    node.bounds.height = height;
    return node;
}

tessera::ui::a11y::A11ySnapshot scene() {
    A11yNode root = element(Role::Generic, "Root", 0.0F, 0.0F, 800.0F, 600.0F);
    A11yNode button = element(Role::Button, "Save", 10.0F, 10.0F, 100.0F, 32.0F);
    button.actions = Action::Press | Action::Focus;
    button.states = State::Focusable | State::Focused;
    button.automation_id = 42;
    A11yNode list = element(Role::List, "Rows", 0.0F, 100.0F, 800.0F, 300.0F);
    A11yNode item = element(Role::ListItem, "Row 1", 0.0F, 100.0F, 800.0F, 40.0F);
    item.actions = Action::Press;
    item.states = State::Selected;
    A11yNode field = element(Role::TextField, "Email", 10.0F, 420.0F, 200.0F, 28.0F);
    field.actions = Action::SetValue | Action::Focus;
    set_text(field.value, "a@b");
    link_children(root, {1, 2, 4});
    link_children(list, {3});
    return snapshot_of({root, button, list, item, field});
}

bool projection() {
    UiaProvider provider;
    tessera::ui::a11y::A11ySnapshot snapshot = scene();
    provider.set_snapshot(snapshot);

    const auto root = provider.element(0);
    const auto button = provider.element(1);
    const auto list = provider.element(2);
    const auto item = provider.element(3);
    const auto field = provider.element(4);
    if (!expect(root.has_value(), "root projects") ||
        !expect(button.has_value(), "button projects") ||
        !expect(list.has_value(), "list projects") || !expect(item.has_value(), "item projects") ||
        !expect(field.has_value(), "field projects")) {
        return false;
    }

    return expect(root->control_type == UiaControlType::Pane, "container is a pane") &&
           expect(root->patterns == UiaPatterns::None, "container exposes no pattern") &&
           expect_equal(name_of(*root), "Root", "root name") &&
           expect(button->control_type == UiaControlType::Button, "button control type") &&
           expect(has_pattern(button->patterns, UiaPatterns::Invoke), "press -> invoke") &&
           expect(button->focused, "focused state") && expect(button->enabled, "enabled state") &&
           expect_eq(static_cast<std::int64_t>(button->automation_id), 42, "automation id") &&
           expect_eq(static_cast<std::int64_t>(button->parent), 0, "button parent") &&
           expect_eq(button->index_in_parent, 0, "button index in parent") &&
           expect(button->bounds.width == 100.0F && button->bounds.height == 32.0F, "bounds") &&
           expect(list->control_type == UiaControlType::List, "list control type") &&
           expect(has_pattern(list->patterns, UiaPatterns::Selection), "list -> selection") &&
           expect(has_pattern(item->patterns, UiaPatterns::SelectionItem),
                  "selected item -> selection item") &&
           expect(has_pattern(item->patterns, UiaPatterns::Invoke), "pressable item -> invoke") &&
           expect(item->selected, "item selected state") &&
           expect(field->control_type == UiaControlType::Edit, "field control type") &&
           expect(has_pattern(field->patterns, UiaPatterns::Value), "field -> value") &&
           expect_equal(value_of(*field), "a@b", "field value");
}

bool navigation() {
    UiaProvider provider;
    tessera::ui::a11y::A11ySnapshot snapshot = scene();
    provider.set_snapshot(snapshot);

    const auto count = provider.child_count(0);
    const auto first = provider.child_at(0, 0);
    const auto second = provider.child_at(0, 1);
    const auto beyond = provider.child_at(0, 3);
    return expect(count.has_value() && *count == 3, "root has three children") &&
           expect(first.has_value() && *first == 1, "first child") &&
           expect(second.has_value() && *second == 2, "second child") &&
           expect(!beyond.has_value() && beyond.error() == UiaError::UnknownNode,
                  "child index is bounded") &&
           expect(provider.parent_of(1) == 0, "parent table") &&
           expect(provider.parent_of(0) == tessera::ui::a11y::kInvalidIndex,
                  "root has no parent") &&
           expect_eq(provider.index_in_parent(3), 0, "nested child index") &&
           expect(provider.element(99).error() == UiaError::UnknownNode, "unknown node") &&
           expect(provider.focused().has_value() && *provider.focused() == 1, "focused node");
}

bool hit_testing() {
    UiaProvider provider;
    tessera::ui::a11y::A11ySnapshot snapshot = scene();
    provider.set_snapshot(snapshot);
    const auto button = provider.hit_test(50.0, 20.0);
    const auto item = provider.hit_test(400.0, 120.0);
    const auto root = provider.hit_test(700.0, 550.0);
    const auto outside = provider.hit_test(900.0, 900.0);
    return expect(button.has_value() && *button == 1, "hit test finds the topmost control") &&
           expect(item.has_value() && *item == 3, "hit test finds a nested row") &&
           expect(root.has_value() && *root == 0, "empty area falls back to the root") &&
           expect(outside.has_value() && *outside == 0, "outside still answers the root");
}

bool empty_snapshot() {
    UiaProvider provider;
    return expect(!provider.has_snapshot(), "no snapshot") &&
           expect(provider.element(0).error() == UiaError::NoSnapshot,
                  "element without snapshot") &&
           expect(provider.focused().error() == UiaError::NoSnapshot, "focus without snapshot") &&
           expect(provider.hit_test(0.0, 0.0).error() == UiaError::NoSnapshot,
                  "hit test without snapshot");
}

bool actions() {
    UiaProvider provider;
    tessera::ui::a11y::A11ySnapshot snapshot = scene();
    provider.set_snapshot(snapshot);

    const auto invoke = provider.request_action(1, Action::Press);
    const auto focus = provider.request_action(1, Action::Focus);
    const auto wrong = provider.request_action(1, Action::SetValue);
    const auto disabled = provider.request_action(3, Action::Scroll);
    if (!expect(invoke.has_value(), "press accepted") ||
        !expect(focus.has_value(), "focus accepted") ||
        !expect(!wrong.has_value() && wrong.error() == UiaError::PatternNotSupported,
                "unsupported action rejected") ||
        !expect(!disabled.has_value() && disabled.error() == UiaError::PatternNotSupported,
                "scroll without the action rejected")) {
        return false;
    }

    // A text payload travels with SetValue; identical requests coalesce on the newest text.
    if (!expect(provider.request_action(4, Action::SetValue, 0.0, "first").has_value(),
                "set value with text") ||
        !expect(provider.request_action(4, Action::SetValue, 0.0, "second").has_value(),
                "set value again") ||
        !expect_eq(static_cast<std::int64_t>(provider.queued_actions()), 3, "three queued") ||
        !expect_eq(static_cast<std::int64_t>(provider.coalesced_actions()), 1, "one coalesced")) {
        return false;
    }
    std::vector<UiaActionRequest> drained(8);
    const std::size_t taken = provider.take_actions(drained);
    return expect_eq(static_cast<std::int64_t>(taken), 3, "all requests drained") &&
           expect(drained[0].action == Action::Press, "first is press") &&
           expect(drained[1].action == Action::Focus, "second is focus") &&
           expect(drained[2].action == Action::SetValue, "third is set value") &&
           expect_equal(text_of(drained[2]), "second", "newest text kept");
}

bool disabled_node() {
    UiaProvider provider;
    A11yNode root = element(Role::Generic, "Root", 0.0F, 0.0F, 100.0F, 100.0F);
    A11yNode button = element(Role::Button, "Off", 0.0F, 0.0F, 100.0F, 40.0F);
    button.actions = Action::Press | Action::Focus;
    button.states = State::Disabled;
    link_children(root, {1});
    tessera::ui::a11y::A11ySnapshot snapshot = snapshot_of({root, button});
    provider.set_snapshot(snapshot);

    const auto press = provider.request_action(1, Action::Press);
    const auto element_info = provider.element(1);
    return expect(!press.has_value() && press.error() == UiaError::ElementNotEnabled,
                  "disabled node rejects actions") &&
           expect(element_info.has_value() && !element_info->enabled, "disabled reported");
}

} // namespace

int main() {
    if (!projection()) {
        return 1;
    }
    if (!navigation()) {
        return 2;
    }
    if (!hit_testing()) {
        return 3;
    }
    if (!empty_snapshot()) {
        return 4;
    }
    if (!actions()) {
        return 5;
    }
    if (!disabled_node()) {
        return 6;
    }
    std::cout << "a11y_uia_provider: ok\n";
    return 0;
}
