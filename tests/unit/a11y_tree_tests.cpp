// SPDX-License-Identifier: MIT
//
// Accessibility semantics snapshot: role/name precedence and the hidden/ignored rules.

#include "a11y_test_helpers.hpp"

namespace a11y_test {
bool role_precedence() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    root.set_element_kind("View");

    Node& explicit_button = child(root, 2);
    explicit_button.set_element_kind("View");
    explicit_button.set_role("button");

    Node& implicit_button = child(root, 3);
    implicit_button.set_element_kind("Button");

    Node& unknown_explicit = child(root, 4);
    unknown_explicit.set_element_kind("Button");
    unknown_explicit.set_role("widget");

    Node& text_input = child(root, 5);
    text_input.set_element_kind("TextInput");

    Node& scroll = child(root, 6);
    scroll.set_element_kind("Scroll");

    child(root, 7);

    StubStyles styles;
    A11ySnapshot snapshot;
    if (!expect(build(root, styles, snapshot).emitted == 7, "all nodes emitted")) {
        return false;
    }
    return expect(snapshot.nodes[1].role == Role::Button, "explicit role wins") &&
           expect(snapshot.nodes[2].role == Role::Button, "element kind implies the role") &&
           expect(snapshot.nodes[3].role == Role::Generic,
                  "unknown explicit role degrades to generic") &&
           expect(snapshot.nodes[4].role == Role::TextField, "TextInput implies textbox") &&
           expect(snapshot.nodes[5].role == Role::ScrollView, "Scroll implies scroll view") &&
           expect(snapshot.nodes[6].role == Role::Generic, "no metadata is generic") &&
           expect(role_from_string("list item") == Role::ListItem, "role spelling list item");
}

bool name_precedence() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});

    Node& labelled = child(root, 2);
    labelled.set_element_kind("Text");
    Node& label_source = child(root, 3);
    label_source.set_element_id("lbl");
    label_source.set_aria("aria-label", "From label");

    Node& by_label = child(root, 4);
    by_label.set_element_kind("Button");
    by_label.set_aria("aria-label", "Close");

    Node& by_ids = child(root, 5);
    by_ids.set_element_kind("Button");
    by_ids.set_aria("aria-labelledby", "missing lbl");

    Node& by_text = child(root, 6);
    by_text.set_element_kind("Text");
    by_text.set_paint_data(text_payload("Hello"));

    Node& by_placeholder = child(root, 7);
    by_placeholder.set_element_kind("TextInput");
    by_placeholder.set_aria("aria-placeholder", "Search");
    by_placeholder.set_element_id("q");

    Node& by_id = child(root, 8);
    by_id.set_element_kind("Button");
    by_id.set_element_id("submit");

    Node& text_field = child(root, 9);
    text_field.set_element_kind("TextInput");
    text_field.set_aria("aria-placeholder", "Search");
    text_field.set_paint_data(text_payload("typed value"));

    StubStyles styles;
    A11ySnapshot snapshot;
    if (!expect(build(root, styles, snapshot).emitted == 9, "all name nodes emitted")) {
        return false;
    }
    return expect(name_of(snapshot.nodes[1]).empty(), "an unlabelled text node is nameless") &&
           expect(name_of(snapshot.nodes[2]) == "From label", "labelledby source name") &&
           expect(name_of(snapshot.nodes[3]) == "Close", "aria-label wins") &&
           expect(name_of(snapshot.nodes[4]) == "From label", "labelledby skips unknown ids") &&
           expect(name_of(snapshot.nodes[5]) == "Hello", "paint text is the next fallback") &&
           expect(name_of(snapshot.nodes[6]) == "Search", "placeholder beats id") &&
           expect(name_of(snapshot.nodes[7]) == "submit", "id is the last fallback") &&
           expect(name_of(snapshot.nodes[8]) == "Search",
                  "a text field uses its placeholder as name") &&
           expect(value_of(snapshot.nodes[8]) == "typed value", "text field value is the text");
}

bool hidden_rules() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& visible = child(root, 2);
    visible.set_bounds({0, 0, 10, 10});

    Node& invisible = child(root, 3);
    invisible.set_bounds({0, 0, 10, 10});
    invisible.set_visible(false);
    child(invisible, 4);

    Node& aria = child(root, 5);
    aria.set_bounds({0, 0, 10, 10});
    aria.set_aria("aria-hidden", "true");
    child(aria, 6);

    Node& displayed = child(root, 7);
    displayed.set_bounds({0, 0, 10, 10});
    Node& unshown = child(root, 8);
    unshown.set_bounds({0, 0, 10, 10});
    Node& zero = child(root, 9);
    zero.set_bounds({0, 0, 0, 10});

    StubStyles styles;
    styles.set(7, styled({{"display", "none"}}));
    styles.set(8, styled({{"visibility", "hidden"}}));

    A11ySnapshot snapshot;
    const BuildStats stats = build(root, styles, snapshot);
    return expect(snapshot.nodes.size() == 2, "hidden subtrees are excluded") &&
           expect(stats.skipped_invisible == 1, "invisible subtree counted") &&
           expect(stats.skipped_aria_hidden == 1, "aria-hidden subtree counted") &&
           expect(stats.skipped_style_hidden == 2, "display/visibility hidden subtrees counted") &&
           expect(stats.skipped_zero_size == 1, "zero-size node counted") &&
           expect(snapshot.nodes[1].id == 2, "only the visible node remains");
}

bool scroll_clipped_out() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& container = child(root, 2);
    container.set_bounds({10, 10, 100, 100});
    container.set_scrollable_y(true);
    container.set_content_size(100.0f, 300.0f);
    container.set_role("scroll view");

    Node& inside = child(container, 3);
    inside.set_bounds({0, 10, 20, 20});
    Node& outside = child(container, 4);
    outside.set_bounds({0, 200, 20, 20});
    Node& partial = child(container, 5);
    partial.set_bounds({0, 90, 20, 40});

    StubStyles styles;
    A11ySnapshot snapshot;
    const BuildStats stats = build(root, styles, snapshot);
    return expect(snapshot.nodes.size() == 4, "clipped-out child excluded") &&
           expect(stats.skipped_clipped == 1, "clipped child counted") &&
           expect(snapshot.nodes[1].role == Role::ScrollView, "scroll container role") &&
           expect(snapshot.nodes[1].child_count == 2, "inside and partial children linked") &&
           expect(snapshot.nodes[3].bounds.height == 40.0f,
                  "partially visible node keeps its full bounds");
}
} // namespace a11y_test

int main() {
    if (!a11y_test::role_precedence()) {
        return 1;
    }
    if (!a11y_test::name_precedence()) {
        return 2;
    }
    if (!a11y_test::hidden_rules()) {
        return 3;
    }
    if (!a11y_test::scroll_clipped_out()) {
        return 4;
    }
    if (!a11y_test::focused_uniqueness()) {
        return 5;
    }
    if (!a11y_test::actions_and_states()) {
        return 6;
    }
    if (!a11y_test::preorder_and_children()) {
        return 7;
    }
    if (!a11y_test::capacity_and_truncated_children()) {
        return 8;
    }
    if (!a11y_test::determinism_and_automation_ids()) {
        return 9;
    }
    if (!a11y_test::combined_scroll_focus_metadata()) {
        return 10;
    }

    std::cout << "a11y_tree: ok\n";
    return 0;
}