// SPDX-License-Identifier: MIT

#include "a11y_role_traits.hpp"
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
    case UiaControlType::DataItem:
        return "data item";
    case UiaControlType::Document:
        return "document";
    case UiaControlType::Window:
        return "window";
    case UiaControlType::Pane:
        return "pane";
    case UiaControlType::HeaderItem:
        return "header item";
    case UiaControlType::Table:
        return "table";
    }
    return "pane";
}

UiaControlType control_type_for(Role role) {
    return detail::role_traits(role).uia;
}

UiaPatterns patterns_for(const A11yNode& node) {
    UiaPatterns patterns = UiaPatterns::None;
    if (has_action(node.actions, Action::Press)) {
        patterns = patterns | UiaPatterns::Invoke;
    }
    if (has_action(node.actions, Action::SetValue)) {
        patterns = patterns | UiaPatterns::Value;
    }
    if (detail::role_traits(node.role).selectable) {
        patterns = patterns | UiaPatterns::SelectionItem;
    }
    // A selected node always answers `SelectionItem.IsSelected`, even when a role tables it
    // differently, because clients read the state to mirror it in their own tree.
    if (has_state(node.states, State::Selected)) {
        patterns = patterns | UiaPatterns::SelectionItem;
    }
    if (detail::role_traits(node.role).selection_container) {
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
