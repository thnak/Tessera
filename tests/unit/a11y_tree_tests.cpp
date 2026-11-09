// SPDX-License-Identifier: MIT
//
// Accessibility semantics snapshot: role/name precedence, hidden/ignored rules, focus uniqueness,
// actions per role, pre-order/children indexing, capacity caps, determinism, and a combined
// scroll + focus + metadata scene.

#include "tessera/ui/accessibility/a11y_tree.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::a11y::A11yIndex;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::BuildOptions;
using tessera::ui::a11y::BuildStats;
using tessera::ui::a11y::kInvalidIndex;
using tessera::ui::a11y::kMaxChildren;
using tessera::ui::a11y::name_of;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::role_from_string;
using tessera::ui::a11y::State;
using tessera::ui::a11y::StyleProvider;
using tessera::ui::a11y::value_of;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::PseudoState;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y_tree: " << message << "\n";
    }
    return condition;
}

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }

    [[nodiscard]] ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

Node& child(Node& parent, NodeId id) {
    auto owner = std::make_unique<Node>(id);
    // A non-zero default size keeps the zero-size skip rule out of tests that only care about
    // semantics; tests override it explicitly when they exercise that rule.
    owner->set_bounds({0.0f, 0.0f, 10.0f, 10.0f});
    return parent.add_child(std::move(owner));
}

tessera::ui::PaintData text_payload(const char* text) {
    tessera::ui::PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = text;
    return data;
}

bool same_node(const A11yNode& lhs, const A11yNode& rhs) {
    return lhs.id == rhs.id && lhs.automation_id == rhs.automation_id && lhs.role == rhs.role &&
           lhs.states == rhs.states && lhs.actions == rhs.actions && name_of(lhs) == name_of(rhs) &&
           value_of(lhs) == value_of(rhs) && lhs.bounds.x == rhs.bounds.x &&
           lhs.bounds.y == rhs.bounds.y && lhs.bounds.width == rhs.bounds.width &&
           lhs.bounds.height == rhs.bounds.height && lhs.children == rhs.children &&
           lhs.child_count == rhs.child_count && lhs.children_truncated == rhs.children_truncated;
}

BuildStats build(Node& root, StubStyles& styles, A11ySnapshot& snapshot,
                 const BuildOptions& options = {}) {
    return tessera::ui::a11y::build_a11y_tree(root, styles, snapshot, options);
}

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

} // namespace

int main() {
    if (!role_precedence()) {
        return 1;
    }
    if (!name_precedence()) {
        return 2;
    }
    if (!hidden_rules()) {
        return 3;
    }
    if (!scroll_clipped_out()) {
        return 4;
    }
    if (!focused_uniqueness()) {
        return 5;
    }
    if (!actions_and_states()) {
        return 6;
    }
    if (!preorder_and_children()) {
        return 7;
    }
    if (!capacity_and_truncated_children()) {
        return 8;
    }
    if (!determinism_and_automation_ids()) {
        return 9;
    }
    if (!combined_scroll_focus_metadata()) {
        return 10;
    }

    std::cout << "a11y_tree: ok\n";
    return 0;
}
