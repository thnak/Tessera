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

bool has(std::int32_t bits, android::A11yAction action) {
    return (bits & static_cast<std::int32_t>(action)) != 0;
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
    check(android::actions_for(Action::None) == 0, "no actions maps to no bits");
    {
        const std::int32_t bits = android::actions_for(Action::Press);
        check(has(bits, android::A11yAction::Click), "press implies click");
        check(has(bits, android::A11yAction::LongClick), "press implies long click");
        check(!has(bits, android::A11yAction::SetText), "press does not imply set text");
    }
    {
        const std::int32_t bits = android::actions_for(Action::SetValue);
        check(has(bits, android::A11yAction::SetText), "set value implies set text");
        check(has(bits, android::A11yAction::SetSelection), "and set selection");
    }
    {
        const std::int32_t bits = android::actions_for(Action::Scroll);
        check(has(bits, android::A11yAction::ScrollForward), "scroll implies scroll forward");
        check(has(bits, android::A11yAction::ScrollBackward), "and scroll backward");
    }
    {
        const std::int32_t bits = android::actions_for(Action::Press | Action::Scroll);
        check(has(bits, android::A11yAction::Click), "a combined set keeps click");
        check(has(bits, android::A11yAction::ScrollForward), "and scroll");
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

    if (failures != 0) {
        std::cerr << "android_a11y: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_a11y: ok\n";
    return 0;
}
