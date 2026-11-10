// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/uia_mapping.hpp"

namespace tessera::ui::a11y {

std::string_view to_string(UiaControlType type) {
    switch (type) {
    case UiaControlType::Button:
        return "button";
    case UiaControlType::CheckBox:
        return "checkbox";
    case UiaControlType::ComboBox:
        return "combobox";
    case UiaControlType::Edit:
        return "edit";
    case UiaControlType::Hyperlink:
        return "hyperlink";
    case UiaControlType::Image:
        return "image";
    case UiaControlType::ListItem:
        return "list item";
    case UiaControlType::List:
        return "list";
    case UiaControlType::Menu:
        return "menu";
    case UiaControlType::MenuItem:
        return "menu item";
    case UiaControlType::ProgressBar:
        return "progress bar";
    case UiaControlType::RadioButton:
        return "radio button";
    case UiaControlType::Slider:
        return "slider";
    case UiaControlType::TabItem:
        return "tab item";
    case UiaControlType::Text:
        return "text";
    case UiaControlType::ToolTip:
        return "tool tip";
    case UiaControlType::Group:
        return "group";
    case UiaControlType::Document:
        return "document";
    case UiaControlType::Window:
        return "window";
    case UiaControlType::Pane:
        return "pane";
    }
    return "pane";
}

UiaControlType control_type_for(Role role) {
    switch (role) {
    case Role::Generic:
    case Role::Presentation:
    case Role::ScrollView:
    case Role::Dialog:
    case Role::TabPanel:
        return UiaControlType::Pane;
    case Role::Button:
        return UiaControlType::Button;
    case Role::Link:
        return UiaControlType::Hyperlink;
    case Role::Text:
    case Role::Heading:
    case Role::Alert:
        return UiaControlType::Text;
    case Role::Image:
        return UiaControlType::Image;
    case Role::TextField:
        return UiaControlType::Edit;
    case Role::Checkbox:
        return UiaControlType::CheckBox;
    case Role::Radio:
        return UiaControlType::RadioButton;
    case Role::Switch:
        // UIA has no switch control type; a binary toggle is a checkbox to clients.
        return UiaControlType::CheckBox;
    case Role::Slider:
        return UiaControlType::Slider;
    case Role::Progress:
        return UiaControlType::ProgressBar;
    case Role::List:
        return UiaControlType::List;
    case Role::ListItem:
        return UiaControlType::ListItem;
    case Role::Tab:
        return UiaControlType::TabItem;
    case Role::Menu:
        return UiaControlType::Menu;
    case Role::MenuItem:
        return UiaControlType::MenuItem;
    case Role::Tooltip:
        return UiaControlType::ToolTip;
    }
    return UiaControlType::Pane;
}

UiaPatterns patterns_for(const A11yNode& node) {
    UiaPatterns patterns = UiaPatterns::None;
    if (has_action(node.actions, Action::Press)) {
        patterns = patterns | UiaPatterns::Invoke;
    }
    if (has_action(node.actions, Action::SetValue)) {
        patterns = patterns | UiaPatterns::Value;
    }
    switch (node.role) {
    case Role::Checkbox:
    case Role::Radio:
    case Role::Switch:
    case Role::Tab:
    case Role::ListItem:
    case Role::MenuItem:
        patterns = patterns | UiaPatterns::SelectionItem;
        break;
    default:
        break;
    }
    // A selected node always answers `SelectionItem.IsSelected`, even when a role tables it
    // differently, because clients read the state to mirror it in their own tree.
    if (has_state(node.states, State::Selected)) {
        patterns = patterns | UiaPatterns::SelectionItem;
    }
    if (node.role == Role::List || node.role == Role::Menu) {
        patterns = patterns | UiaPatterns::Selection;
    }
    return patterns;
}

std::int32_t pattern_id(UiaPatterns pattern) {
    switch (pattern) {
    case UiaPatterns::Invoke:
        return kUiaInvokePatternId;
    case UiaPatterns::Value:
        return kUiaValuePatternId;
    case UiaPatterns::SelectionItem:
        return kUiaSelectionItemPatternId;
    case UiaPatterns::Selection:
        return kUiaSelectionPatternId;
    case UiaPatterns::None:
        break;
    }
    return 0;
}

} // namespace tessera::ui::a11y
