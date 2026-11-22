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

std::int32_t actions_for(Action actions) noexcept {
    std::int32_t bits = 0;
    if (tessera::ui::a11y::has_action(actions, Action::Focus)) {
        bits |= static_cast<std::int32_t>(A11yAction::Focus);
    }
    if (tessera::ui::a11y::has_action(actions, Action::Press)) {
        bits |= static_cast<std::int32_t>(A11yAction::Click);
        bits |= static_cast<std::int32_t>(A11yAction::LongClick);
    }
    if (tessera::ui::a11y::has_action(actions, Action::SetValue)) {
        bits |= static_cast<std::int32_t>(A11yAction::SetText);
        bits |= static_cast<std::int32_t>(A11yAction::SetSelection);
    }
    if (tessera::ui::a11y::has_action(actions, Action::Scroll)) {
        bits |= static_cast<std::int32_t>(A11yAction::ScrollForward);
        bits |= static_cast<std::int32_t>(A11yAction::ScrollBackward);
    }
    return bits;
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

} // namespace tessera::hal::android
