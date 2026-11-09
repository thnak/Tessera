// SPDX-License-Identifier: MIT
//
// Accessibility semantics snapshot: focus, states/actions, pre-order and children indexing,
// capacity/truncation, determinism, and the combined scroll + focus + metadata scene.

#include "a11y_test_helpers.hpp"

namespace a11y_test {
bool focused_uniqueness() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    Node& first = child(root, 2);
    first.set_focusable(true);
    Node& second = child(root, 3);
    second.set_focusable(true);
    Node& third = child(root, 4);
    third.set_focusable(true);

    StubStyles styles;
    A11ySnapshot snapshot;
    BuildOptions options;
    options.focused_id = second.id();
    build(root, styles, snapshot, options);
    std::size_t focused = 0;
    for (const A11yNode& node : snapshot.nodes) {
        if (tessera::ui::a11y::has_state(node.states, State::Focused)) {
            ++focused;
        }
    }
    if (!expect(focused == 1, "exactly one focused node") ||
        !expect(tessera::ui::a11y::has_state(snapshot.nodes[2].states, State::Focused),
                "the requested node is focused")) {
        return false;
    }

    // A hidden focus target produces no focused flag at all (never a ghost focus).
    second.set_visible(false);
    build(root, styles, snapshot, options);
    for (const A11yNode& node : snapshot.nodes) {
        if (!expect(!tessera::ui::a11y::has_state(node.states, State::Focused),
                    "hidden focus target is not exposed")) {
            return false;
        }
    }
    return true;
}

bool actions_and_states() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& button = child(root, 2);
    button.set_element_kind("Button");
    button.set_focusable(true);
    button.set_bounds({0, 0, 20, 20});

    Node& field = child(root, 3);
    field.set_element_kind("TextInput");
    field.set_focusable(true);
    field.set_paint_data(text_payload("abc"));
    field.set_bounds({0, 30, 20, 20});

    Node& scroll = child(root, 4);
    scroll.set_element_kind("Scroll");
    scroll.set_bounds({0, 60, 50, 50});
    scroll.set_scrollable_x(true);
    scroll.set_content_size(100.0f, 50.0f);

    Node& disabled = child(root, 5);
    disabled.set_element_kind("Button");
    disabled.set_focusable(true);
    disabled.set_pseudo_state(PseudoState::Disabled);
    disabled.set_bounds({0, 120, 20, 20});

    Node& decorated = child(root, 6);
    decorated.set_element_kind("Button");
    decorated.set_aria("aria-selected", "true");
    decorated.set_aria("aria-checked", "mixed");
    decorated.set_aria("aria-expanded", "true");
    decorated.set_bounds({0, 150, 20, 20});

    StubStyles styles;
    A11ySnapshot snapshot;
    build(root, styles, snapshot);

    const A11yNode& button_node = snapshot.nodes[1];
    const A11yNode& field_node = snapshot.nodes[2];
    const A11yNode& scroll_node = snapshot.nodes[3];
    const A11yNode& disabled_node = snapshot.nodes[4];
    const A11yNode& decorated_node = snapshot.nodes[5];
    return expect(tessera::ui::a11y::has_action(button_node.actions, Action::Press) &&
                      tessera::ui::a11y::has_action(button_node.actions, Action::Focus),
                  "button exposes press and focus") &&
           expect(tessera::ui::a11y::has_action(field_node.actions, Action::SetValue),
                  "text field exposes set value") &&
           expect(tessera::ui::a11y::has_action(scroll_node.actions, Action::Scroll),
                  "scroll container exposes scroll") &&
           expect(disabled_node.actions == Action::None, "disabled node exposes no actions") &&
           expect(tessera::ui::a11y::has_state(disabled_node.states, State::Disabled),
                  "disabled state") &&
           expect(tessera::ui::a11y::has_state(decorated_node.states, State::Selected) &&
                      tessera::ui::a11y::has_state(decorated_node.states, State::Checked) &&
                      tessera::ui::a11y::has_state(decorated_node.states, State::Expanded),
                  "aria states");
}

bool preorder_and_children() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& a = child(root, 2);
    Node& b = child(root, 3);
    Node& c = child(root, 4);
    child(a, 5);
    child(b, 6);
    child(c, 7);

    StubStyles styles;
    A11ySnapshot snapshot;
    build(root, styles, snapshot);
    if (!expect(snapshot.nodes.size() == 7, "seven nodes") ||
        !expect(snapshot.nodes[0].id == 1, "root first") ||
        !expect(snapshot.nodes[1].id == 2, "pre-order child a") ||
        !expect(snapshot.nodes[2].id == 5, "pre-order grandchild a1") ||
        !expect(snapshot.nodes[3].id == 3, "pre-order child b") ||
        !expect(snapshot.nodes[4].id == 6, "pre-order grandchild b1") ||
        !expect(snapshot.nodes[5].id == 4, "pre-order child c")) {
        return false;
    }
    if (!expect(snapshot.nodes[0].child_count == 3, "root has three children") ||
        !expect(snapshot.nodes[0].children[0] == 1 && snapshot.nodes[0].children[1] == 3 &&
                    snapshot.nodes[0].children[2] == 5,
                "root children indices") ||
        !expect(snapshot.nodes[1].child_count == 1 && snapshot.nodes[1].children[0] == 2,
                "child a links its grandchild")) {
        return false;
    }
    const A11yIndex found = tessera::ui::a11y::find_by_node_id(snapshot, 5);
    return expect(found == 2, "find_by_node_id") &&
           expect(tessera::ui::a11y::find_by_node_id(snapshot, 99) == kInvalidIndex,
                  "unknown node id");
}

bool capacity_and_truncated_children() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    for (NodeId id = 2; id <= 4; ++id) {
        child(root, id);
    }

    StubStyles styles;
    A11ySnapshot snapshot;
    BuildOptions options;
    options.max_nodes = 2;
    const BuildStats stats = build(root, styles, snapshot, options);
    if (!expect(snapshot.nodes.size() == 2, "snapshot stops at the cap") ||
        !expect(stats.truncated, "truncated flag") ||
        !expect(stats.dropped_nodes >= 1, "dropped nodes counted") ||
        !expect(stats.emitted == 2, "emitted count")) {
        return false;
    }

    Node wide(10);
    wide.set_bounds({0, 0, 200, 200});
    for (NodeId id = 11; id <= 11 + kMaxChildren; ++id) {
        child(wide, id);
    }
    A11ySnapshot wide_snapshot;
    const BuildStats wide_stats = build(wide, styles, wide_snapshot);
    return expect(wide_snapshot.nodes[0].child_count == kMaxChildren, "children capped") &&
           expect(wide_snapshot.nodes[0].children_truncated, "child truncation flagged") &&
           expect(wide_stats.truncated_children == 1, "child truncation counted");
}

bool determinism_and_automation_ids() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    Node& a = child(root, 2);
    a.set_element_kind("Button");
    a.set_automation_id(42);
    a.set_aria("aria-label", "Play");
    a.set_bounds({0, 0, 10, 10});
    Node& b = child(root, 3);
    b.set_element_id("target");
    b.set_bounds({0, 20, 10, 10});

    StubStyles styles;
    A11ySnapshot first;
    A11ySnapshot second;
    build(root, styles, first);
    build(root, styles, second);
    if (!expect(first.nodes.size() == second.nodes.size(), "same build size")) {
        return false;
    }
    for (std::size_t index = 0; index < first.nodes.size(); ++index) {
        if (!expect(same_node(first.nodes[index], second.nodes[index]), "builds are identical")) {
            return false;
        }
    }
    return expect(tessera::ui::a11y::find_by_automation_id(first, 42) == 1,
                  "find_by_automation_id") &&
           expect(tessera::ui::a11y::find_by_automation_id(first, 0) == kInvalidIndex,
                  "automation id 0 is invalid");
}

bool combined_scroll_focus_metadata() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& container = child(root, 2);
    container.set_bounds({0, 0, 100, 100});
    container.set_scrollable_y(true);
    container.set_content_size(100.0f, 300.0f);
    container.set_element_kind("Scroll");
    container.set_automation_id(7);

    Node& target = child(container, 3);
    target.set_bounds({0, 150, 20, 20});
    target.set_element_kind("Button");
    target.set_focusable(true);
    target.set_automation_id(8);
    target.set_paint_data(text_payload("Confirm"));

    tessera::ui::scroll_to(container, 0.0f, 70.0f);

    StubStyles styles;
    A11ySnapshot snapshot;
    BuildOptions options;
    options.focused_id = target.id();
    const BuildStats stats = build(root, styles, snapshot, options);
    if (!expect(stats.emitted == 3, "combined scene emits three nodes") ||
        !expect(snapshot.nodes[0].role == Role::Generic, "root role")) {
        return false;
    }
    const A11yNode& scroll_node = snapshot.nodes[1];
    const A11yNode& button_node = snapshot.nodes[2];
    return expect(scroll_node.role == Role::ScrollView, "scroll role") &&
           expect(scroll_node.automation_id == 7, "scroll automation id") &&
           expect(tessera::ui::a11y::has_action(scroll_node.actions, Action::Scroll),
                  "scroll action") &&
           expect(button_node.role == Role::Button, "button role") &&
           expect(name_of(button_node) == "Confirm", "paint text becomes the name") &&
           expect(tessera::ui::a11y::has_state(button_node.states, State::Focused),
                  "focused flag") &&
           expect(button_node.bounds.y == 80.0f, "bounds include the scroll offset") &&
           expect(scroll_node.child_count == 1 && scroll_node.children[0] == 2, "child link");
}
} // namespace a11y_test