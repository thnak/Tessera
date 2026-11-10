// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/atspi_mapping.hpp"

#include <algorithm>
#include <charconv>
#include <string>

namespace tessera::ui::a11y {
namespace {

bool pressable(Role role) {
    switch (role) {
    case Role::Button:
    case Role::Checkbox:
    case Role::Radio:
    case Role::Switch:
    case Role::Link:
    case Role::Tab:
    case Role::MenuItem:
        return true;
    default:
        return false;
    }
}

bool textual(Role role) {
    switch (role) {
    case Role::TextField:
    case Role::Text:
    case Role::Heading:
    case Role::Link:
    case Role::Alert:
    case Role::Tooltip:
        return true;
    default:
        return false;
    }
}

void set_bit(StateSet& states, AtspiState state) {
    const auto index = static_cast<std::uint32_t>(state);
    if (index < 32U) {
        states.low |= 1U << index;
    } else {
        states.high |= 1U << (index - 32U);
    }
}

} // namespace

AtspiRole atspi_role(Role role) {
    switch (role) {
    case Role::Generic:
        return AtspiRole::Filler;
    case Role::Presentation:
        return AtspiRole::Filler;
    case Role::Button:
        return AtspiRole::PushButton;
    case Role::Link:
        return AtspiRole::Link;
    case Role::Text:
        return AtspiRole::Label;
    case Role::Heading:
        return AtspiRole::Heading;
    case Role::Image:
        return AtspiRole::Image;
    case Role::TextField:
        return AtspiRole::Entry;
    case Role::Checkbox:
        return AtspiRole::CheckBox;
    case Role::Radio:
        return AtspiRole::RadioButton;
    case Role::Switch:
        return AtspiRole::ToggleButton;
    case Role::Slider:
        return AtspiRole::Slider;
    case Role::Progress:
        return AtspiRole::ProgressBar;
    case Role::List:
        return AtspiRole::List;
    case Role::ListItem:
        return AtspiRole::ListItem;
    case Role::Tab:
        return AtspiRole::PageTab;
    case Role::TabPanel:
        return AtspiRole::Panel;
    case Role::Dialog:
        return AtspiRole::Dialog;
    case Role::Menu:
        return AtspiRole::Menu;
    case Role::MenuItem:
        return AtspiRole::MenuItem;
    case Role::ScrollView:
        return AtspiRole::ScrollPane;
    case Role::Tooltip:
        return AtspiRole::ToolTip;
    case Role::Alert:
        return AtspiRole::Alert;
    }
    return AtspiRole::Unknown;
}

std::string_view atspi_role_name(Role role) {
    switch (role) {
    case Role::Generic:
        return "filler";
    case Role::Presentation:
        return "filler";
    case Role::Button:
        return "push button";
    case Role::Link:
        return "link";
    case Role::Text:
        return "label";
    case Role::Heading:
        return "heading";
    case Role::Image:
        return "image";
    case Role::TextField:
        return "entry";
    case Role::Checkbox:
        return "check box";
    case Role::Radio:
        return "radio button";
    case Role::Switch:
        return "toggle button";
    case Role::Slider:
        return "slider";
    case Role::Progress:
        return "progress bar";
    case Role::List:
        return "list";
    case Role::ListItem:
        return "list item";
    case Role::Tab:
        return "page tab";
    case Role::TabPanel:
        return "panel";
    case Role::Dialog:
        return "dialog";
    case Role::Menu:
        return "menu";
    case Role::MenuItem:
        return "menu item";
    case Role::ScrollView:
        return "scroll pane";
    case Role::Tooltip:
        return "tool tip";
    case Role::Alert:
        return "alert";
    }
    return "unknown";
}

StateSet atspi_states(const A11yNode& node) {
    StateSet states;
    // The snapshot only contains nodes that are visible and laid out, so these are unconditional.
    set_bit(states, AtspiState::Showing);
    set_bit(states, AtspiState::Visible);
    const bool enabled = !has_state(node.states, State::Disabled);
    if (enabled) {
        set_bit(states, AtspiState::Enabled);
        set_bit(states, AtspiState::Sensitive);
    }
    if (has_state(node.states, State::Focusable)) {
        set_bit(states, AtspiState::Focusable);
    }
    if (has_state(node.states, State::Focused)) {
        set_bit(states, AtspiState::Focused);
    }
    if (has_state(node.states, State::Selected)) {
        set_bit(states, AtspiState::Selectable);
        set_bit(states, AtspiState::Selected);
    }
    if (has_state(node.states, State::Checked)) {
        set_bit(states, AtspiState::Checkable);
        set_bit(states, AtspiState::Checked);
    }
    if (has_state(node.states, State::Expanded)) {
        set_bit(states, AtspiState::Expandable);
        set_bit(states, AtspiState::Expanded);
    }
    if (node.role == Role::TextField) {
        set_bit(states, AtspiState::Editable);
    }
    return states;
}

bool has_state(const StateSet& states, AtspiState state) {
    const auto index = static_cast<std::uint32_t>(state);
    if (index >= 32U) {
        return (states.high & (1U << (index - 32U))) != 0;
    }
    return (states.low & (1U << index)) != 0;
}

InterfaceSet atspi_interfaces(const A11yNode& node, bool is_root) {
    InterfaceSet interfaces;
    interfaces.application = is_root;
    interfaces.action = atspi_action_count(node) > 0;
    interfaces.text = textual(node.role) && !atspi_text(node).empty();
    interfaces.value = atspi_value(node).has_value;
    return interfaces;
}

std::size_t atspi_interface_names(const InterfaceSet& interfaces, std::span<std::string_view> out) {
    std::size_t count = 0;
    auto add = [&out, &count](std::string_view name) {
        if (count < out.size()) {
            out[count] = name;
            ++count;
        }
    };
    if (interfaces.accessible) {
        add(kAccessibleInterface);
    }
    if (interfaces.action) {
        add(kActionInterface);
    }
    if (interfaces.application) {
        add(kApplicationInterface);
    }
    if (interfaces.component) {
        add(kComponentInterface);
    }
    if (interfaces.text) {
        add(kTextInterface);
    }
    if (interfaces.value) {
        add(kValueInterface);
    }
    return count;
}

std::uint32_t atspi_action_count(const A11yNode& node) {
    return has_action(node.actions, Action::Press) && pressable(node.role) ? 1U : 0U;
}

std::string_view atspi_action_name(const A11yNode& node, std::uint32_t index) {
    if (index == 0 && atspi_action_count(node) > 0) {
        return "click";
    }
    return {};
}

std::string_view atspi_text(const A11yNode& node) {
    if (node.role == Role::TextField) {
        return value_of(node);
    }
    return name_of(node);
}

AtspiValue atspi_value(const A11yNode& node) {
    AtspiValue value;
    if (node.role != Role::Slider && node.role != Role::Progress) {
        return value;
    }
    value.has_value = true;
    const std::string_view text = value_of(node);
    if (!text.empty()) {
        double parsed = 0.0;
        const char* const first = text.data();
        const char* const last = first + text.size();
        const auto [end, error] = std::from_chars(first, last, parsed);
        if (error == std::errc() && end == last) {
            value.current = parsed;
        }
    }
    if (value.current < value.minimum) {
        value.current = value.minimum;
    }
    if (value.current > value.maximum) {
        value.current = value.maximum;
    }
    return value;
}

AtspiLayer atspi_layer(const A11yNode& node) {
    switch (node.role) {
    case Role::Dialog:
    case Role::Menu:
    case Role::MenuItem:
    case Role::Tooltip:
    case Role::Alert:
        return AtspiLayer::Popup;
    case Role::Button:
    case Role::Link:
    case Role::Text:
    case Role::Heading:
    case Role::Image:
    case Role::TextField:
    case Role::Checkbox:
    case Role::Radio:
    case Role::Switch:
    case Role::Slider:
    case Role::Progress:
    case Role::Tab:
        return AtspiLayer::Widget;
    default:
        return AtspiLayer::Background;
    }
}

std::size_t format_object_path(A11yIndex index, std::span<char> out) {
    const std::string text = std::string(kAccessiblePathPrefix) + std::to_string(index);
    if (out.size() <= text.size()) {
        return 0;
    }
    std::copy(text.begin(), text.end(), out.begin());
    out[text.size()] = '\0';
    return text.size();
}

A11yIndex parse_object_path(std::string_view path) {
    if (!path.starts_with(kAccessiblePathPrefix)) {
        return kInvalidIndex;
    }
    const std::string_view digits = path.substr(kAccessiblePathPrefix.size());
    if (digits.empty()) {
        return kInvalidIndex;
    }
    std::uint32_t value = 0;
    for (const char digit : digits) {
        if (digit < '0' || digit > '9') {
            return kInvalidIndex;
        }
        value = value * 10U + static_cast<std::uint32_t>(digit - '0');
        if (value >= kInvalidIndex) {
            return kInvalidIndex;
        }
    }
    return static_cast<A11yIndex>(value);
}

} // namespace tessera::ui::a11y
