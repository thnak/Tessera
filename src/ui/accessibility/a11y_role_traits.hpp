// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"
#include "tessera/ui/accessibility/atspi_mapping.hpp"
#include "tessera/ui/accessibility/uia_mapping.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::ui::a11y::detail {

// One row per `Role`: the single place that knows what a semantics role means to every consumer.
// Before this table the meaning was spread over nine `switch (role)` blocks in four files
// (a11y_roles.cpp, atspi_mapping.cpp, uia_mapping.cpp, a11y_tree.cpp), four of which ended in a
// `default` arm, so adding an ARIA role silently produced a generic value in the ones nobody
// remembered to edit. Here a missing row is a compile error, not a wrong answer
// (docs/architecture/god-type-audit-v1.md section 4.1).
//
// This header lives under src/ rather than beside `Role` in the public a11y_tree.hpp because the
// mapping columns need `AtspiRole`, `AtspiLayer` and `UiaControlType`, and those headers already
// include a11y_tree.hpp; putting the table in the public base header would create a cycle. It is
// internal to `tessera_accessibility`, so no installed header grows a Windows/AT-SPI dependency.
struct RoleTraits {
    // ARIA spelling reported by `to_string` and accepted by `role_from_string`.
    std::string_view name;
    // AT-SPI2 role enum and its English `GetRoleName` stand-in.
    AtspiRole atspi{AtspiRole::Unknown};
    std::string_view atspi_name;
    // Windows UI Automation control type.
    UiaControlType uia{UiaControlType::Pane};
    // AT-SPI2 component layer: screen readers use it for modality decisions.
    AtspiLayer layer{AtspiLayer::Background};
    // Derives `Action::Press` (and therefore AT-SPI2's action count and UIA's Invoke pattern).
    bool press{false};
    // Derives `Action::SetValue` (and therefore UIA's Value pattern).
    bool set_value{false};
    // The role carries readable text, so AT-SPI2 advertises the Text interface.
    bool textual{false};
    // Selectable item: UIA advertises `SelectionItem`.
    bool selectable{false};
    // A container that owns a selection: UIA advertises `Selection`.
    bool selection_container{false};
    // Text entry: the value string is the text content and AT-SPI2 reports Editable.
    bool editable{false};
    // Carries a numeric value: AT-SPI2 advertises the Value interface.
    bool range_value{false};
};

// Order must match `Role`; every accessor below indexes this array directly.
inline constexpr std::array<RoleTraits, static_cast<std::size_t>(Role::kCount)> kRoleTable = {{
    // Generic
    {.name = "generic",
     .atspi = AtspiRole::Filler,
     .atspi_name = "filler",
     .uia = UiaControlType::Pane,
     .layer = AtspiLayer::Background},
    // Presentation (decorative)
    {.name = "presentation",
     .atspi = AtspiRole::Filler,
     .atspi_name = "filler",
     .uia = UiaControlType::Pane,
     .layer = AtspiLayer::Background},
    // Button
    {.name = "button",
     .atspi = AtspiRole::PushButton,
     .atspi_name = "push button",
     .uia = UiaControlType::Button,
     .layer = AtspiLayer::Widget,
     .press = true},
    // Link
    {.name = "link",
     .atspi = AtspiRole::Link,
     .atspi_name = "link",
     .uia = UiaControlType::Hyperlink,
     .layer = AtspiLayer::Widget,
     .press = true,
     .textual = true},
    // Text
    {.name = "text",
     .atspi = AtspiRole::Label,
     .atspi_name = "label",
     .uia = UiaControlType::Text,
     .layer = AtspiLayer::Widget,
     .textual = true},
    // Heading
    {.name = "heading",
     .atspi = AtspiRole::Heading,
     .atspi_name = "heading",
     .uia = UiaControlType::Text,
     .layer = AtspiLayer::Widget,
     .textual = true},
    // Image
    {.name = "img",
     .atspi = AtspiRole::Image,
     .atspi_name = "image",
     .uia = UiaControlType::Image,
     .layer = AtspiLayer::Widget},
    // TextField
    {.name = "textbox",
     .atspi = AtspiRole::Entry,
     .atspi_name = "entry",
     .uia = UiaControlType::Edit,
     .layer = AtspiLayer::Widget,
     .set_value = true,
     .textual = true,
     .editable = true},
    // Checkbox
    {.name = "checkbox",
     .atspi = AtspiRole::CheckBox,
     .atspi_name = "check box",
     .uia = UiaControlType::CheckBox,
     .layer = AtspiLayer::Widget,
     .press = true,
     .selectable = true},
    // Radio
    {.name = "radio",
     .atspi = AtspiRole::RadioButton,
     .atspi_name = "radio button",
     .uia = UiaControlType::RadioButton,
     .layer = AtspiLayer::Widget,
     .press = true,
     .selectable = true},
    // Switch: UIA has no switch control type, so a binary toggle is a checkbox to clients.
    {.name = "switch",
     .atspi = AtspiRole::ToggleButton,
     .atspi_name = "toggle button",
     .uia = UiaControlType::CheckBox,
     .layer = AtspiLayer::Widget,
     .press = true,
     .selectable = true},
    // Slider
    {.name = "slider",
     .atspi = AtspiRole::Slider,
     .atspi_name = "slider",
     .uia = UiaControlType::Slider,
     .layer = AtspiLayer::Widget,
     .set_value = true,
     .range_value = true},
    // Progress
    {.name = "progress",
     .atspi = AtspiRole::ProgressBar,
     .atspi_name = "progress bar",
     .uia = UiaControlType::ProgressBar,
     .layer = AtspiLayer::Widget,
     .range_value = true},
    // List
    {.name = "list",
     .atspi = AtspiRole::List,
     .atspi_name = "list",
     .uia = UiaControlType::List,
     .layer = AtspiLayer::Background,
     .selection_container = true},
    // ListItem
    {.name = "listitem",
     .atspi = AtspiRole::ListItem,
     .atspi_name = "list item",
     .uia = UiaControlType::ListItem,
     .layer = AtspiLayer::Background,
     .selectable = true},
    // Tab
    {.name = "tab",
     .atspi = AtspiRole::PageTab,
     .atspi_name = "page tab",
     .uia = UiaControlType::TabItem,
     .layer = AtspiLayer::Widget,
     .press = true,
     .selectable = true},
    // TabPanel
    {.name = "tabpanel",
     .atspi = AtspiRole::Panel,
     .atspi_name = "panel",
     .uia = UiaControlType::Pane,
     .layer = AtspiLayer::Background},
    // Dialog
    {.name = "dialog",
     .atspi = AtspiRole::Dialog,
     .atspi_name = "dialog",
     .uia = UiaControlType::Pane,
     .layer = AtspiLayer::Popup},
    // Menu
    {.name = "menu",
     .atspi = AtspiRole::Menu,
     .atspi_name = "menu",
     .uia = UiaControlType::Menu,
     .layer = AtspiLayer::Popup,
     .selection_container = true},
    // MenuItem
    {.name = "menuitem",
     .atspi = AtspiRole::MenuItem,
     .atspi_name = "menu item",
     .uia = UiaControlType::MenuItem,
     .layer = AtspiLayer::Popup,
     .press = true,
     .selectable = true},
    // ScrollView
    {.name = "scroll view",
     .atspi = AtspiRole::ScrollPane,
     .atspi_name = "scroll pane",
     .uia = UiaControlType::Pane,
     .layer = AtspiLayer::Background},
    // Tooltip
    {.name = "tooltip",
     .atspi = AtspiRole::ToolTip,
     .atspi_name = "tool tip",
     .uia = UiaControlType::ToolTip,
     .layer = AtspiLayer::Popup,
     .textual = true},
    // Alert: UIA exposes it as Text so its name is read like a live region.
    {.name = "alert",
     .atspi = AtspiRole::Alert,
     .atspi_name = "alert",
     .uia = UiaControlType::Text,
     .layer = AtspiLayer::Popup,
     .textual = true},
}};

static_assert(kRoleTable.size() == static_cast<std::size_t>(Role::kCount),
              "kRoleTable needs exactly one row per Role value");

// Extra spellings `role_from_string` accepts besides the canonical `name` above. Kept apart so the
// table above stays one row per role.
struct RoleAlias {
    std::string_view name;
    Role role;
};

inline constexpr RoleAlias kRoleAliases[] = {
    {"image", Role::Image},         {"text field", Role::TextField},
    {"textfield", Role::TextField}, {"progressbar", Role::Progress},
    {"list item", Role::ListItem},  {"tab panel", Role::TabPanel},
    {"menu item", Role::MenuItem},  {"scrollview", Role::ScrollView},
    {"none", Role::Presentation},
};

// Row for `role`. An out-of-range value (only reachable by casting an arbitrary integer) degrades
// to Generic, which is what every mapper returned for an unknown role before the table existed.
[[nodiscard]] constexpr const RoleTraits& role_traits(Role role) noexcept {
    const auto index = static_cast<std::size_t>(role);
    return index < kRoleTable.size() ? kRoleTable[index] : kRoleTable.front();
}

} // namespace tessera::ui::a11y::detail
