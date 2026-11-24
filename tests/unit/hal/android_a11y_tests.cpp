// SPDX-License-Identifier: MIT
//
// The Android accessibility mapping (issue #189): no Android SDK, no JNI, no device.
//
// What it proves is what a device lane cannot see cheaply: that a decorative node is skipped, that
// a text field's content is reported as its value rather than its label, and that the enabled flag
// is the inverse of the engine's disabled state.

#include "android_a11y_mapping.hpp"

#include <iostream>

namespace {

namespace android = tessera::hal::android;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::State;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

// A node with a role and no text.
A11yNode node_of(Role role) {
    A11yNode node;
    node.role = role;
    return node;
}

// Fills `name`/`value` with short ASCII strings, which is what the snapshot carries.
void set_text(A11yNode& node, const char* name, const char* value = "") {
    std::size_t i = 0;
    for (; name[i] != '\0' && i < node.name.size(); ++i) {
        node.name[i] = name[i];
    }
    i = 0;
    for (; value[i] != '\0' && i < node.value.size(); ++i) {
        node.value[i] = value[i];
    }
}

bool has(const android::A11yActionList& list, android::A11yAction action) {
    return android::actions_contain(list, action);
}

} // namespace

int main() {
    // --- The role-to-class-name table ---------------------------------------------------------
    check(std::string_view(android::class_name_for(Role::Button)) == "android.widget.Button",
          "a button announces as the platform's button");
    check(std::string_view(android::class_name_for(Role::TextField)) == "android.widget.EditText",
          "a text field announces as an edit text");
    check(std::string_view(android::class_name_for(Role::Checkbox)) == "android.widget.CheckBox",
          "a checkbox announces as a checkbox");
    check(std::string_view(android::class_name_for(Role::ScrollView)) ==
              "android.widget.ScrollView",
          "a scroll view announces as one");
    // A decorative node has no class name: it is not emitted.
    check(android::class_name_for(Role::Presentation) == nullptr,
          "a decorative node has no class name");

    // --- Emission -----------------------------------------------------------------------------
    check(!android::emits_node(Role::Presentation), "a decorative node is not emitted");
    check(android::emits_node(Role::Button), "a button is emitted");
    check(android::emits_node(Role::Generic), "a generic node is emitted");
    // kCount is not a role; emitting it would be a bug.
    check(!android::emits_node(Role::kCount), "the count sentinel is not emitted");

    // --- Actions ------------------------------------------------------------------------------
    // The platform's ids are wire values, not ours: ACTION_FOCUS is 1 and ACTION_CLICK is 16, so a
    // mapping that numbered them in enum order would turn a click into a focus request. These
    // assertions pin the real values so a refactor cannot quietly renumber them.
    check(static_cast<std::int32_t>(android::A11yAction::Focus) == 1, "ACTION_FOCUS is 1");
    check(static_cast<std::int32_t>(android::A11yAction::Click) == 16, "ACTION_CLICK is 16");
    check(static_cast<std::int32_t>(android::A11yAction::LongClick) == 32,
          "ACTION_LONG_CLICK is 32");
    check(static_cast<std::int32_t>(android::A11yAction::ScrollForward) == 4096,
          "ACTION_SCROLL_FORWARD is 4096");
    check(static_cast<std::int32_t>(android::A11yAction::SetText) == 2097152,
          "ACTION_SET_TEXT is 2097152");
    check(android::actions_for(Action::None).count == 0, "no actions maps to no ids");
    {
        const android::A11yActionList list = android::actions_for(Action::Press);
        check(has(list, android::A11yAction::Click), "press implies click");
        check(has(list, android::A11yAction::LongClick), "press implies long click");
        check(!has(list, android::A11yAction::SetText), "press does not imply set text");
    }
    {
        const android::A11yActionList list = android::actions_for(Action::SetValue);
        check(has(list, android::A11yAction::SetText), "set value implies set text");
        check(has(list, android::A11yAction::SetSelection), "and set selection");
    }
    {
        const android::A11yActionList list = android::actions_for(Action::Scroll);
        check(has(list, android::A11yAction::ScrollForward), "scroll implies scroll forward");
        check(has(list, android::A11yAction::ScrollBackward), "and scroll backward");
    }
    {
        const android::A11yActionList list = android::actions_for(Action::Press | Action::Scroll);
        check(has(list, android::A11yAction::Click), "a combined set keeps click");
        check(has(list, android::A11yAction::ScrollForward), "and scroll");
    }
    // The largest set one node can carry must fit the array, or a control would silently lose an
    // action and a screen reader would stop offering it.
    {
        const android::A11yActionList all =
            android::actions_for(Action::Press | Action::Focus | Action::SetValue | Action::Scroll);
        check(!all.overflowed, "the full action set fits the list");
        check(all.count == 7, "and produces seven ids");
    }

    // --- States -------------------------------------------------------------------------------
    // Disabled is the one state whose sense is inverted: the engine records "disabled", the
    // platform asks "enabled". Getting this backwards leaves a disabled control usable by a screen
    // reader.
    {
        A11yNode node = node_of(Role::Button);
        check(android::describe(node).enabled, "a node with no Disabled state is enabled");
        node.states = State::Disabled;
        check(!android::describe(node).enabled, "a Disabled node is not enabled");
    }
    {
        A11yNode node = node_of(Role::Button);
        node.states = State::Focusable;
        check(android::describe(node).focusable, "Focusable makes it focusable");
        node.states = State::Focused;
        check(android::describe(node).focusable, "and so does Focused");
    }
    {
        A11yNode node = node_of(Role::ListItem);
        node.states = State::Selected;
        check(android::describe(node).selected, "Selected is carried");
    }

    // --- Checkable is derived from the ROLE, not only the state -------------------------------
    // A TextView with a stale Checked bit must not announce as a checkbox.
    {
        A11yNode text = node_of(Role::Text);
        text.states = State::Checked;
        const android::A11yNodeInfo info = android::describe(text);
        check(!info.checkable, "text is not checkable even with a Checked bit");
        check(!info.checked, "and does not report checked");
    }
    {
        A11yNode box = node_of(Role::Checkbox);
        box.states = State::Checked;
        const android::A11yNodeInfo info = android::describe(box);
        check(info.checkable, "a checkbox is checkable");
        check(info.checked, "and reports its checked state");
    }

    // --- A text field reports its content as the VALUE ---------------------------------------
    // Reporting the typed content as the name makes a screen reader announce it as the control's
    // label, which is the wrong side of the announcement.
    {
        A11yNode field = node_of(Role::TextField);
        set_text(field, "Email", "me@example.com");
        const android::A11yNodeInfo info = android::describe(field);
        check(info.editable, "a text field is editable");
        check(info.text == "Email", "the name is the label");
        check(info.description == "me@example.com", "the typed content is the value");
    }
    // A non-editable node keeps its description as the description.
    {
        A11yNode button = node_of(Role::Button);
        set_text(button, "Save", "ignored");
        const android::A11yNodeInfo info = android::describe(button);
        check(!info.editable, "a button is not editable");
        check(info.text == "Save", "the name is the text");
    }

    // --- Scrolling ----------------------------------------------------------------------------
    {
        A11yNode list = node_of(Role::List);
        list.actions = Action::Scroll;
        check(android::describe(list).scrollable, "a node with a scroll action is scrollable");
        check(!android::describe(node_of(Role::List)).scrollable,
              "a list without the action is not");
    }

    // --- The reverse direction: what a screen reader sends back -------------------------------
    // This is the direction that fails silently: the platform echoes the id it was offered, so a
    // wrong answer here is a control that does nothing when double-tapped.
    {
        using android::A11yAction;
        check(android::engine_action_for(static_cast<std::int32_t>(A11yAction::Click)) ==
                  Action::Press,
              "a click is a press");
        check(android::engine_action_for(static_cast<std::int32_t>(A11yAction::LongClick)) ==
                  Action::Press,
              "and so is a long click, which the engine has no separate dispatch for");
        // Not a power of two, and not adjacent to Click: the id has to be read, not guessed.
        check(android::engine_action_for(static_cast<std::int32_t>(A11yAction::ScrollForward)) ==
                  Action::Scroll,
              "scroll forward is a scroll");
        check(android::engine_action_for(static_cast<std::int32_t>(A11yAction::SetText)) ==
                  Action::SetValue,
              "set text is a value write");
        check(android::engine_action_for(static_cast<std::int32_t>(A11yAction::Focus)) ==
                  Action::Focus,
              "focus is a focus");
        // The platform's own bookkeeping is not an engine action, and faking one would move focus
        // the app never asked to move.
        check(android::engine_action_for(static_cast<std::int32_t>(A11yAction::Select)) ==
                  Action::None,
              "select is not an engine action");
        check(android::engine_action_for(static_cast<std::int32_t>(A11yAction::ClearSelection)) ==
                  Action::None,
              "and neither is clear selection");
        check(android::engine_action_for(0) == Action::None, "no id is no action");
        // An id the platform does not define must not become an action by accident.
        check(android::engine_action_for(12345) == Action::None, "an unknown id is no action");
    }

    // --- The round trip ----------------------------------------------------------------------
    // Every id the engine offers must map back to an engine action, or a screen reader is offered
    // something that does nothing when activated.
    {
        using android::A11yAction;
        const Action all = Action::Press | Action::Focus | Action::SetValue | Action::Scroll;
        const android::A11yActionList offered = android::actions_for(all);
        for (std::size_t i = 0; i < offered.count; ++i) {
            const auto id = static_cast<std::int32_t>(offered.actions[i]);
            check(android::engine_action_for(id) != Action::None,
                  "every offered action id maps back to an engine action");
        }
    }

    // --- A collection item reports where it sits ---------------------------------------------
    // The platform turns these into `CollectionItemInfo`/`CollectionInfo`, which is what makes a
    // screen reader say "3 of 40" (accessibility spec section 5).
    {
        A11yNode item = node_of(Role::ListItem);
        item.collection.set_size = 40;
        item.collection.pos_in_set = 3;
        const android::A11yNodeInfo described = android::describe(item);
        check(described.is_collection_item, "a node in a collection is a collection item");
        check(described.pos_in_set == 3, "and reports its 1-based position");
        check(described.set_size == 40, "and the collection's item count");

        // A node outside one is not an item, and the flag is what says so: an empty collection has
        // `set_size == 0` too, so the size alone cannot distinguish the two.
        A11yNode plain = node_of(Role::ListItem);
        const android::A11yNodeInfo plain_info = android::describe(plain);
        check(!plain_info.is_collection_item, "a node outside a collection is not an item");
        check(plain_info.set_size == 0, "and has no set size");
    }

    if (failures != 0) {
        std::cerr << "android_a11y: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_a11y: ok\n";
    return 0;
}
