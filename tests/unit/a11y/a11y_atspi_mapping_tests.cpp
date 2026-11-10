// SPDX-License-Identifier: MIT
//
// AT-SPI2 mapping: role ids/names, the 64-bit state set, interface lists, action names, text
// content, the numeric Value surface, component layers, and object path round-trips.

#include "a11y_dbus_test_support.hpp"
#include "tessera/ui/accessibility/atspi_bridge.hpp"
#include "tessera/ui/accessibility/atspi_mapping.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace {

using namespace a11y_dbus_test;
using tessera::rendering::rhi::Rect;
using tessera::ui::a11y::A11yIndex;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::atspi_action_count;
using tessera::ui::a11y::atspi_action_name;
using tessera::ui::a11y::atspi_interface_names;
using tessera::ui::a11y::atspi_interfaces;
using tessera::ui::a11y::atspi_layer;
using tessera::ui::a11y::atspi_role;
using tessera::ui::a11y::atspi_role_name;
using tessera::ui::a11y::atspi_states;
using tessera::ui::a11y::atspi_text;
using tessera::ui::a11y::atspi_value;
using tessera::ui::a11y::AtspiLayer;
using tessera::ui::a11y::AtspiRole;
using tessera::ui::a11y::AtspiState;
using tessera::ui::a11y::format_object_path;
using tessera::ui::a11y::has_state;
using tessera::ui::a11y::kInvalidIndex;
using tessera::ui::a11y::kRootObjectPath;
using tessera::ui::a11y::parse_object_path;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::State;

A11yNode make_node(Role role, std::string_view name, std::string_view value = {}) {
    A11yNode node;
    node.role = role;
    std::copy(name.begin(), name.end(), node.name.begin());
    std::copy(value.begin(), value.end(), node.value.begin());
    node.bounds = Rect{10.0f, 20.0f, 30.0f, 40.0f};
    return node;
}

bool roles() {
    return expect(atspi_role(Role::Button) == AtspiRole::PushButton, "button role") &&
           expect(atspi_role(Role::TextField) == AtspiRole::Entry, "text field is entry") &&
           expect(atspi_role(Role::Text) == AtspiRole::Label, "text is label") &&
           expect(atspi_role(Role::Image) == AtspiRole::Image, "image role") &&
           expect(atspi_role(Role::Checkbox) == AtspiRole::CheckBox, "check box role") &&
           expect(atspi_role(Role::Switch) == AtspiRole::ToggleButton, "switch is toggle button") &&
           expect(atspi_role(Role::TabPanel) == AtspiRole::Panel, "tab panel is panel") &&
           expect(atspi_role(Role::Generic) == AtspiRole::Filler, "generic is filler") &&
           expect(atspi_role(Role::Alert) == AtspiRole::Alert, "alert role") &&
           expect(static_cast<std::uint32_t>(AtspiRole::PushButton) == 42, "push button id") &&
           expect(static_cast<std::uint32_t>(AtspiRole::Entry) == 76, "entry id") &&
           expect_equal(atspi_role_name(Role::Button), "push button", "role name") &&
           expect_equal(atspi_role_name(Role::Progress), "progress bar", "progress role name");
}

bool state_bits() {
    A11yNode node = make_node(Role::Checkbox, "Remember me");
    node.states = State::Focusable | State::Focused | State::Checked;
    const auto states = atspi_states(node);
    if (!expect(has_state(states, AtspiState::Visible), "visible bit") ||
        !expect(has_state(states, AtspiState::Showing), "showing bit") ||
        !expect(has_state(states, AtspiState::Enabled), "enabled bit") ||
        !expect(has_state(states, AtspiState::Sensitive), "sensitive bit") ||
        !expect(has_state(states, AtspiState::Focusable), "focusable bit") ||
        !expect(has_state(states, AtspiState::Focused), "focused bit") ||
        !expect(has_state(states, AtspiState::Checked), "checked bit") ||
        !expect(has_state(states, AtspiState::Checkable), "checkable bit")) {
        return false;
    }
    if (!expect((states.low & (1U << 26)) != 0, "showing is bit 26") ||
        !expect((states.high & (1U << 9)) != 0, "checkable is bit 41 -> high bit 9")) {
        return false;
    }
    A11yNode disabled = make_node(Role::Button, "Save");
    disabled.states = State::Disabled;
    const auto disabled_states = atspi_states(disabled);
    return expect(!has_state(disabled_states, AtspiState::Enabled), "disabled clears enabled") &&
           expect(!has_state(disabled_states, AtspiState::Sensitive), "disabled clears sensitive");
}

bool interfaces() {
    const A11yNode button = make_node(Role::Button, "Close");
    A11yNode pressable = button;
    pressable.actions = Action::Press | Action::Focus;
    const auto button_interfaces = atspi_interfaces(pressable, false);
    if (!expect(button_interfaces.accessible, "accessible is always exposed") ||
        !expect(button_interfaces.component, "component is always exposed") ||
        !expect(button_interfaces.action, "pressable exposes action") ||
        !expect(!button_interfaces.text, "a button exposes no text interface") ||
        !expect(!button_interfaces.application, "only the root is an application")) {
        return false;
    }
    const A11yNode field = make_node(Role::TextField, "Search", "query");
    const auto field_interfaces = atspi_interfaces(field, false);
    if (!expect(field_interfaces.text, "a text field exposes text") ||
        !expect(!field_interfaces.value, "a text field exposes no value")) {
        return false;
    }
    A11yNode slider = make_node(Role::Slider, "Volume", "40");
    const auto slider_interfaces = atspi_interfaces(slider, false);
    if (!expect(slider_interfaces.value, "a slider exposes value")) {
        return false;
    }
    const auto root_interfaces = atspi_interfaces(make_node(Role::Generic, "Root"), true);
    if (!expect(root_interfaces.application, "the root is an application")) {
        return false;
    }
    std::array<std::string_view, 6> names{};
    const std::size_t count = atspi_interface_names(button_interfaces, names);
    return expect(count == 3, "accessible+action+component") &&
           expect_equal(names[0], "org.a11y.atspi.Accessible", "first interface") &&
           expect_equal(names[1], "org.a11y.atspi.Action", "second interface") &&
           expect_equal(names[2], "org.a11y.atspi.Component", "third interface");
}

bool actions() {
    A11yNode button = make_node(Role::Button, "Close");
    button.actions = Action::Press;
    A11yNode field = make_node(Role::TextField, "Search");
    field.actions = Action::SetValue;
    A11yNode slider = make_node(Role::Slider, "Volume");
    slider.actions = Action::Press | Action::SetValue;
    return expect(atspi_action_count(button) == 1, "button has one action") &&
           expect_equal(atspi_action_name(button, 0), "click", "button action name") &&
           expect(atspi_action_name(button, 1).empty(), "no second action") &&
           expect(atspi_action_count(field) == 0, "set value is not an AT-SPI2 action") &&
           expect(atspi_action_count(slider) == 0, "value roles use the Value interface");
}

bool texts_and_values() {
    const A11yNode label = make_node(Role::Text, "Hello");
    const A11yNode field = make_node(Role::TextField, "Search", "typed text");
    const A11yNode slider = make_node(Role::Slider, "Volume", "37.5");
    const A11yNode progress = make_node(Role::Progress, "Loading", "not-a-number");
    const A11yNode button = make_node(Role::Button, "Save", "ignored");

    const auto slider_value = atspi_value(slider);
    const auto progress_value = atspi_value(progress);
    return expect_equal(atspi_text(label), "Hello", "label text is its name") &&
           expect_equal(atspi_text(field), "typed text", "text field text is its value") &&
           expect(slider_value.has_value && slider_value.current == 37.5, "slider current value") &&
           expect(slider_value.minimum == 0.0 && slider_value.maximum == 100.0, "slider range") &&
           expect(progress_value.has_value && progress_value.current == 0.0,
                  "unparsable progress value is zero") &&
           expect(!atspi_value(button).has_value, "a button has no value");
}

bool value_clamping() {
    A11yNode high = make_node(Role::Progress, "Loading", "250");
    A11yNode low = make_node(Role::Slider, "Volume", "-4");
    return expect(atspi_value(high).current == 100.0, "values above maximum clamp") &&
           expect(atspi_value(low).current == 0.0, "values below minimum clamp");
}

bool layers() {
    return expect(atspi_layer(make_node(Role::Dialog, "Confirm")) == AtspiLayer::Popup,
                  "dialogs are popups") &&
           expect(atspi_layer(make_node(Role::Button, "OK")) == AtspiLayer::Widget,
                  "buttons are widgets") &&
           expect(atspi_layer(make_node(Role::List, "Items")) == AtspiLayer::Background,
                  "lists sit in the background layer");
}

bool object_paths() {
    std::array<char, 48> path{};
    const std::size_t written = format_object_path(0, path);
    if (!expect(written == kRootObjectPath.size(), "root path length") ||
        !expect_equal(path.data(), "/org/a11y/atspi/accessible/0", "root path") ||
        !expect(parse_object_path(kRootObjectPath) == 0, "root index round-trip")) {
        return false;
    }
    std::array<char, 48> child{};
    (void)format_object_path(37, child);
    if (!expect_equal(child.data(), "/org/a11y/atspi/accessible/37", "child path") ||
        !expect(parse_object_path(child.data()) == 37, "child index round-trip")) {
        return false;
    }
    // Rejections: foreign paths, trailing segments, non-digits, out-of-range indices.
    if (!expect(parse_object_path("/org/a11y/atspi/accessible/") == kInvalidIndex, "no index") ||
        !expect(parse_object_path("/org/a11y/atspi/accessible/1/2") == kInvalidIndex,
                "nested path") ||
        !expect(parse_object_path("/org/a11y/atspi/accessible/1x") == kInvalidIndex, "non-digit") ||
        !expect(parse_object_path("/org/a11y/atspi/accessible/70000") == kInvalidIndex,
                "index overflow") ||
        !expect(parse_object_path("/org/example/accessible/1") == kInvalidIndex,
                "foreign prefix")) {
        return false;
    }
    std::array<char, 8> tiny{};
    return expect(format_object_path(1, tiny) == 0, "undersized buffer reports no write");
}

} // namespace

int main() {
    if (!roles()) {
        return 1;
    }
    if (!state_bits()) {
        return 2;
    }
    if (!interfaces()) {
        return 3;
    }
    if (!actions()) {
        return 4;
    }
    if (!texts_and_values()) {
        return 5;
    }
    if (!value_clamping()) {
        return 6;
    }
    if (!layers()) {
        return 7;
    }
    if (!object_paths()) {
        return 8;
    }
    std::cout << "a11y_atspi_mapping: ok\n";
    return 0;
}
