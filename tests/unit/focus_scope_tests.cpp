// SPDX-License-Identifier: MIT

// Focus scope (trap and restore) and roving focus (issue #236), headless through the InputRouter:
// the nodes are built the way generated code builds a `focusTrap` / `focusGroup` View.

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <iostream>
#include <memory>
#include <string_view>

namespace {

using tessera::ui::FocusManager;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
namespace key = tessera::ui::key;

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "focus_scope: " << message << "\n";
        ++failures;
    }
}

Node& add(Node& parent, tessera::ui::NodeId id, bool focusable = true) {
    Node& node = parent.add_child(std::make_unique<Node>(id));
    node.set_focusable(focusable);
    node.set_tab_index(0);
    return node;
}

void press(InputRouter& router, Node& root, tessera::ui::KeyCode code, bool shift = false) {
    InputEvent event;
    event.type = InputEventType::KeyDown;
    event.key = code;
    if (shift) {
        event.modifiers = tessera::ui::Modifier::Shift;
    }
    router.dispatch(root, event);
}

// A trap confines Tab to its subtree, and hiding it restores the focus held before it opened.
void test_trap_confines_tab_and_restores_focus() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& opener = add(root, 2);
    Node& other = add(root, 3);
    Node& dialog = add(root, 4, false);
    dialog.set_focus_trap(true);
    Node& ok = add(dialog, 5);
    Node& cancel = add(dialog, 6);
    dialog.set_visible(false);
    InputRouter router;
    FocusManager& focus = router.focus_manager();
    focus.set_root(root);
    focus.request_focus(&opener);
    focus.sync_focus_trap(root);
    check(focus.focused() == &opener, "a hidden scope changes nothing");

    dialog.set_visible(true);
    focus.sync_focus_trap(root);
    check(focus.focused() == &ok, "showing the scope moves focus into it");
    press(router, root, key::kTab);
    check(focus.focused() == &cancel, "Tab moves inside the scope");
    press(router, root, key::kTab);
    check(focus.focused() == &ok, "Tab wraps inside the scope, never to the page");
    check(!focus.allows_focus(other), "the page behind the scope is not focusable");

    dialog.set_visible(false);
    focus.sync_focus_trap(root);
    check(focus.focused() == &opener, "hiding the scope restores the previous focus");
    press(router, root, key::kTab);
    check(focus.focused() == &other, "Tab reaches the page again");
}

// A nested scope wins over its ancestor and releases back to it.
void test_nested_scopes() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& outer = add(root, 2, false);
    outer.set_focus_trap(true);
    Node& outer_item = add(outer, 3);
    Node& inner = add(outer, 4, false);
    inner.set_focus_trap(true);
    Node& inner_item = add(inner, 5);
    inner.set_visible(false);
    FocusManager focus;
    focus.set_root(root);
    focus.sync_focus_trap(root);
    check(focus.focused() == &outer_item, "the outer scope takes focus");
    inner.set_visible(true);
    focus.sync_focus_trap(root);
    check(focus.focused() == &inner_item, "the nested scope takes focus");
    inner.set_visible(false);
    focus.sync_focus_trap(root);
    check(focus.focused() == &outer_item, "closing the nested scope returns to the outer one");
}

// A group is one Tab stop; arrows of its axis move among the items and wrap; Home and End jump.
void test_roving_focus() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& before = add(root, 2);
    Node& group = add(root, 3, false);
    group.set_focus_group(Node::FocusGroup::Horizontal);
    Node& a = add(group, 4);
    Node& b = add(group, 5);
    Node& c = add(group, 6);
    Node& after = add(root, 7);
    InputRouter router;
    FocusManager& focus = router.focus_manager();
    focus.set_root(root);

    press(router, root, key::kTab);
    check(focus.focused() == &before, "Tab reaches the control before the group");
    press(router, root, key::kTab);
    check(focus.focused() == &a, "Tab enters the group at its first item");
    press(router, root, key::kTab);
    check(focus.focused() == &after, "the group is one Tab stop");

    focus.request_focus(&a);
    press(router, root, key::kArrowRight);
    check(focus.focused() == &b, "ArrowRight moves to the next item");
    press(router, root, key::kArrowDown);
    check(focus.focused() == &b, "an arrow of the other axis does nothing");
    press(router, root, key::kEnd);
    check(focus.focused() == &c, "End moves to the last item");
    press(router, root, key::kArrowRight);
    check(focus.focused() == &a, "ArrowRight wraps to the first item");
    press(router, root, key::kArrowLeft);
    check(focus.focused() == &c, "ArrowLeft wraps to the last item");
    press(router, root, key::kHome);
    check(focus.focused() == &a, "Home moves to the first item");

    focus.request_focus(&c);
    focus.request_focus(&after);
    press(router, root, key::kTab, true);
    check(focus.focused() == &c, "Shift+Tab re-enters at the item that was last active");
}

void test_vertical_and_both_axes() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& column = add(root, 2, false);
    column.set_focus_group(Node::FocusGroup::Vertical);
    Node& first = add(column, 3);
    Node& second = add(column, 4);
    InputRouter router;
    router.focus_manager().set_root(root);
    router.focus_manager().request_focus(&first);
    press(router, root, key::kArrowRight);
    check(router.focus_manager().focused() == &first, "a vertical group ignores ArrowRight");
    press(router, root, key::kArrowDown);
    check(router.focus_manager().focused() == &second, "a vertical group takes ArrowDown");
    column.set_focus_group(Node::FocusGroup::Both);
    press(router, root, key::kArrowLeft);
    check(router.focus_manager().focused() == &first, "a both-axes group takes ArrowLeft");
}

// Items carry their label as `aria-label`, borrowed from static storage like generated code.
Node& add_item(Node& parent, tessera::ui::NodeId id, std::string_view label) {
    Node& node = add(parent, id);
    node.set_aria("aria-label", label);
    return node;
}

void type(InputRouter& router, Node& root, char c, double time_s) {
    InputEvent event;
    event.type = InputEventType::Text;
    event.text_codepoint = static_cast<std::uint32_t>(c);
    event.timestamp_s = time_s;
    router.dispatch(root, event);
}

void test_typeahead() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& list = add(root, 2, false);
    list.set_focus_group(Node::FocusGroup::Vertical);
    list.set_typeahead(true);
    Node& apple = add_item(list, 3, "Apple");
    Node& banana = add_item(list, 4, "Banana");
    Node& blueberry = add_item(list, 5, "Blueberry");
    Node& cherry = add_item(list, 6, "Cherry");
    InputRouter router;
    FocusManager& focus = router.focus_manager();
    focus.set_root(root);
    focus.request_focus(&apple);

    type(router, root, 'c', 10.0);
    check(focus.focused() == &cherry, "a character jumps to the item that starts with it");
    type(router, root, 'b', 20.0);
    check(focus.focused() == &banana, "the first match after the focused item wins (wraps)");
    type(router, root, 'b', 20.2);
    check(focus.focused() == &blueberry, "a repeated character cycles through its items");
    type(router, root, 'b', 20.4);
    check(focus.focused() == &banana, "and wraps around");
    type(router, root, 'l', 30.0);
    check(focus.focused() == &banana, "no item starts with `l`: focus stays");
    type(router, root, 'b', 40.0);
    type(router, root, 'l', 40.3);
    check(focus.focused() == &blueberry, "a growing prefix narrows the match");
    type(router, root, 'A', 60.0);
    check(focus.focused() == &apple, "matching ignores case, and the prefix reset after a second");

    list.set_typeahead(false);
    type(router, root, 'c', 70.0);
    check(focus.focused() == &apple, "typeahead is off without the flag");
}

} // namespace

int main() {
    test_typeahead();
    test_trap_confines_tab_and_restores_focus();
    test_nested_scopes();
    test_roving_focus();
    test_vertical_and_both_axes();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "focus scope tests passed\n";
    return 0;
}
