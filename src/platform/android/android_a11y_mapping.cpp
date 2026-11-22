// SPDX-License-Identifier: MIT
//
// The Android accessibility mapping (issue #189, docs/design/mobile-integration.md section 3.4).
//
// No Android header, no <jni.h>: the role-to-class-name table and the action bits are the
// platform's stable API values, restated so the mapping is unit-tested on every lane.

#include "android_a11y_mapping.hpp"

namespace tessera::hal::android {
namespace {

using tessera::ui::a11y::Action;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::State;

} // namespace

const char* class_name_for(Role role) noexcept {
    // One switch with no `default`, so a new Role is a compile error here rather than a node that
    // silently announces as a generic view. The names are the platform's own widget classes: a
    // screen reader announces `Button` as a button and `EditText` as a text field, and a custom
    // class name would announce as nothing useful.
    switch (role) {
    case Role::Generic:
        return "android.view.View";
    case Role::Presentation:
        // Decorative: not emitted at all. Returning nullptr is how `describe` says so, and
        // `emits_node` is the explicit check a caller uses before creating a node.
        return nullptr;
    case Role::Button:
        return "android.widget.Button";
    case Role::Link:
        // A link is a clickable text, and the platform has no dedicated widget class for it.
        return "android.widget.TextView";
    case Role::Text:
        return "android.widget.TextView";
    case Role::Heading:
        // A heading is text with a collection-item trait; the platform reports the heading bit
        // separately (there is no Heading widget).
        return "android.widget.TextView";
    case Role::Image:
        return "android.widget.ImageView";
    case Role::TextField:
        return "android.widget.EditText";
    case Role::Checkbox:
        return "android.widget.CheckBox";
    case Role::Radio:
        return "android.widget.RadioButton";
    case Role::Switch:
        return "android.widget.Switch";
    case Role::Slider:
        return "android.widget.SeekBar";
    case Role::Progress:
        return "android.widget.ProgressBar";
    case Role::List:
        return "android.widget.ListView";
    case Role::ListItem:
        return "android.widget.TextView";
    case Role::Tab:
        return "android.widget.TabWidget";
    case Role::TabPanel:
        return "android.view.ViewGroup";
    case Role::Dialog:
        return "android.app.Dialog";
    case Role::Menu:
        return "android.widget.PopupMenu";
    case Role::MenuItem:
        return "android.view.MenuItem";
    case Role::ScrollView:
        return "android.widget.ScrollView";
    case Role::Tooltip:
        return "android.widget.Tooltip";
    case Role::Alert:
        return "android.app.AlertDialog";
    case Role::Paragraph:
        return "android.widget.TextView";
    case Role::kCount:
        break;
    }
    // `kCount` is not a real role; reaching here means a cast from an arbitrary integer. The
    // generic view is the safe answer, matching what the role table's own fallback does.
    return "android.view.View";
}

A11yActionList actions_for(Action actions) noexcept {
    // A list because the platform's ids are not powers of two: see `A11yActionList`. The order is
    // fixed so a test can assert the sequence, and so the Java side adds them deterministically.
    A11yActionList out;
    auto add = [&out](A11yAction action) {
        if (out.count == out.actions.size()) {
            out.overflowed = true;
            return;
        }
        out.actions[out.count] = action;
        ++out.count;
    };
    if (tessera::ui::a11y::has_action(actions, Action::Press)) {
        add(A11yAction::Click);
        add(A11yAction::LongClick);
    }
    if (tessera::ui::a11y::has_action(actions, Action::Focus)) {
        // Only the ids the engine can honour. `ClearFocus` is deliberately NOT offered: the engine
        // has no "unfocus" that does not leave the focus manager inconsistent, so offering it would
        // promise a screen reader something that silently does nothing. The round-trip test in
        // android_a11y_tests enforces that every id offered maps back to an engine action.
        add(A11yAction::Focus);
    }
    if (tessera::ui::a11y::has_action(actions, Action::SetValue)) {
        add(A11yAction::SetText);
        add(A11yAction::SetSelection);
    }
    if (tessera::ui::a11y::has_action(actions, Action::Scroll)) {
        add(A11yAction::ScrollForward);
        add(A11yAction::ScrollBackward);
    }
    return out;
}

bool actions_contain(const A11yActionList& list, A11yAction action) noexcept {
    for (std::size_t i = 0; i < list.count; ++i) {
        if (list.actions[i] == action) {
            return true;
        }
    }
    return false;
}

Action engine_action_for(std::int32_t platform_action) noexcept {
    // No `default`: a new A11yAction is a compile error here rather than an action a screen reader
    // offers and the engine silently ignores.
    switch (static_cast<A11yAction>(platform_action)) {
    case A11yAction::Click:
    case A11yAction::LongClick:
        // A long click is a press in the engine: it has no separate long-press dispatch, and
        // activating the control is what the screen reader asked for either way.
        return Action::Press;
    case A11yAction::Focus:
    case A11yAction::ClearFocus:
        // A focus request is a real focus move; clearing it is the platform's own bookkeeping and
        // the engine has no "unfocus" that makes sense outside its focus manager.
        return platform_action == static_cast<std::int32_t>(A11yAction::Focus) ? Action::Focus
                                                                               : Action::None;
    case A11yAction::AccessibilityFocus:
    case A11yAction::ClearAccessibilityFocus:
        // Accessibility focus is a separate concept from engine focus, and the engine has no
        // second focus ring. Offering it as focus keeps TalkBack's swipe-to-focus working; clearing
        // it is ignored.
        return platform_action == static_cast<std::int32_t>(A11yAction::AccessibilityFocus)
                   ? Action::Focus
                   : Action::None;
    case A11yAction::SetText:
    case A11yAction::SetSelection:
        return Action::SetValue;
    case A11yAction::ScrollForward:
    case A11yAction::ScrollBackward:
        return Action::Scroll;
    case A11yAction::None:
    case A11yAction::Select:
    case A11yAction::ClearSelection:
        break;
    }
    return Action::None;
}

bool emits_node(Role role) noexcept {
    // A decorative node is skipped but its children are not, which is what Presentation means.
    return role != Role::Presentation && role != Role::kCount;
}

A11yNodeInfo describe(const ui::a11y::A11yNode& node) noexcept {
    A11yNodeInfo out;
    out.class_name = class_name_for(node.role);
    out.text = ui::a11y::name_of(node);
    out.description = ui::a11y::description_of(node);
    out.focusable = tessera::ui::a11y::has_state(node.states, State::Focusable) ||
                    tessera::ui::a11y::has_state(node.states, State::Focused);
    // Disabled is the one state whose sense is inverted on the platform: the engine records
    // "disabled", the platform asks "enabled".
    out.enabled = !tessera::ui::a11y::has_state(node.states, State::Disabled);
    out.selected = tessera::ui::a11y::has_state(node.states, State::Selected);
    // Only a role that can be checked reports checkable: a TextView with a stale Checked bit must
    // not announce as a checkbox.
    out.checkable =
        node.role == Role::Checkbox || node.role == Role::Radio || node.role == Role::Switch;
    out.checked = out.checkable && tessera::ui::a11y::has_state(node.states, State::Checked);
    out.editable = node.role == Role::TextField;
    out.scrollable = tessera::ui::a11y::has_action(node.actions, Action::Scroll);
    out.actions = actions_for(node.actions);
    // A text field's content is its VALUE on the platform, and its label is the name. Reporting the
    // text as the name instead makes a screen reader announce the typed content as the control's
    // label, which is the wrong side of the announcement.
    if (out.editable) {
        out.description = ui::a11y::value_of(node);
        out.text = ui::a11y::name_of(node);
    }
    return out;
}

A11yNodeInfo describe_node(const ui::a11y::ProviderAnswer& answer) noexcept {
    // The provider has already applied the engine's rules (the enabled inversion, the role-derived
    // checkable, the value/label split), so this only names the role and copies the action list.
    // Duplicating the rules here would give the push and pull paths two chances to disagree.
    A11yNodeInfo out;
    out.class_name = class_name_for(answer.role);
    out.text = answer.text;
    out.description = answer.description;
    out.focusable = answer.focusable;
    out.enabled = answer.enabled;
    out.selected = answer.selected;
    out.checkable = answer.checkable;
    out.checked = answer.checked;
    out.editable = answer.editable;
    out.scrollable = answer.scrollable;
    out.actions = actions_for(answer.actions);
    return out;
}

} // namespace tessera::hal::android
