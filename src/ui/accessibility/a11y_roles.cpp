// SPDX-License-Identifier: MIT

#include "a11y_text.hpp"
#include "tessera/ui/accessibility/a11y_tree.hpp"

namespace tessera::ui::a11y {
std::string_view to_string(Role role) {
    switch (role) {
    case Role::Generic:
        return "generic";
    case Role::Presentation:
        return "presentation";
    case Role::Button:
        return "button";
    case Role::Link:
        return "link";
    case Role::Text:
        return "text";
    case Role::Heading:
        return "heading";
    case Role::Image:
        return "img";
    case Role::TextField:
        return "textbox";
    case Role::Checkbox:
        return "checkbox";
    case Role::Radio:
        return "radio";
    case Role::Switch:
        return "switch";
    case Role::Slider:
        return "slider";
    case Role::Progress:
        return "progress";
    case Role::List:
        return "list";
    case Role::ListItem:
        return "listitem";
    case Role::Tab:
        return "tab";
    case Role::TabPanel:
        return "tabpanel";
    case Role::Dialog:
        return "dialog";
    case Role::Menu:
        return "menu";
    case Role::MenuItem:
        return "menuitem";
    case Role::ScrollView:
        return "scroll view";
    case Role::Tooltip:
        return "tooltip";
    case Role::Alert:
        return "alert";
    }
    return "generic";
}

Role role_from_string(std::string_view name) {
    const std::string_view value = detail::trim(name);
    if (detail::equal_ci(value, "button")) {
        return Role::Button;
    }
    if (detail::equal_ci(value, "link")) {
        return Role::Link;
    }
    if (detail::equal_ci(value, "text")) {
        return Role::Text;
    }
    if (detail::equal_ci(value, "heading")) {
        return Role::Heading;
    }
    if (detail::equal_ci(value, "img") || detail::equal_ci(value, "image")) {
        return Role::Image;
    }
    if (detail::equal_ci(value, "textbox") || detail::equal_ci(value, "text field") ||
        detail::equal_ci(value, "textfield")) {
        return Role::TextField;
    }
    if (detail::equal_ci(value, "checkbox")) {
        return Role::Checkbox;
    }
    if (detail::equal_ci(value, "radio")) {
        return Role::Radio;
    }
    if (detail::equal_ci(value, "switch")) {
        return Role::Switch;
    }
    if (detail::equal_ci(value, "slider")) {
        return Role::Slider;
    }
    if (detail::equal_ci(value, "progress") || detail::equal_ci(value, "progressbar")) {
        return Role::Progress;
    }
    if (detail::equal_ci(value, "list")) {
        return Role::List;
    }
    if (detail::equal_ci(value, "listitem") || detail::equal_ci(value, "list item")) {
        return Role::ListItem;
    }
    if (detail::equal_ci(value, "tab")) {
        return Role::Tab;
    }
    if (detail::equal_ci(value, "tabpanel") || detail::equal_ci(value, "tab panel")) {
        return Role::TabPanel;
    }
    if (detail::equal_ci(value, "dialog")) {
        return Role::Dialog;
    }
    if (detail::equal_ci(value, "menu")) {
        return Role::Menu;
    }
    if (detail::equal_ci(value, "menuitem") || detail::equal_ci(value, "menu item")) {
        return Role::MenuItem;
    }
    if (detail::equal_ci(value, "scroll view") || detail::equal_ci(value, "scrollview")) {
        return Role::ScrollView;
    }
    if (detail::equal_ci(value, "tooltip")) {
        return Role::Tooltip;
    }
    if (detail::equal_ci(value, "alert")) {
        return Role::Alert;
    }
    if (detail::equal_ci(value, "presentation") || detail::equal_ci(value, "none")) {
        return Role::Presentation;
    }
    return Role::Generic;
}

} // namespace tessera::ui::a11y