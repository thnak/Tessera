// SPDX-License-Identifier: MIT
//
// Role/state/action -> UI Automation control type, pattern set, and id mapping. These are the
// platform-neutral tables the Windows COM adapter reads; the same header compiles on every
// platform, which is half of the "headers are guarded" check.

#include "a11y_uia_test_support.hpp"
#include "tessera/ui/accessibility/uia_mapping.hpp"

#include <array>
#include <string_view>

namespace {

using namespace a11y_uia_test;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::control_type_for;
using tessera::ui::a11y::has_pattern;
using tessera::ui::a11y::kUiaInvokePatternId;
using tessera::ui::a11y::kUiaSelectionItemPatternId;
using tessera::ui::a11y::kUiaSelectionPatternId;
using tessera::ui::a11y::kUiaValuePatternId;
using tessera::ui::a11y::pattern_id;
using tessera::ui::a11y::patterns_for;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::State;
using tessera::ui::a11y::to_string;
using tessera::ui::a11y::UiaControlType;
using tessera::ui::a11y::UiaPatterns;

bool control_types() {
    struct Case {
        Role role;
        UiaControlType type;
    };
    constexpr std::array<Case, 24> kCases{{
        {Role::Generic, UiaControlType::Pane},
        {Role::Presentation, UiaControlType::Pane},
        {Role::Button, UiaControlType::Button},
        {Role::Link, UiaControlType::Hyperlink},
        {Role::Text, UiaControlType::Text},
        {Role::Heading, UiaControlType::Text},
        {Role::Image, UiaControlType::Image},
        {Role::TextField, UiaControlType::Edit},
        {Role::Checkbox, UiaControlType::CheckBox},
        {Role::Radio, UiaControlType::RadioButton},
        {Role::Switch, UiaControlType::CheckBox},
        {Role::Slider, UiaControlType::Slider},
        {Role::Progress, UiaControlType::ProgressBar},
        {Role::List, UiaControlType::List},
        {Role::ListItem, UiaControlType::ListItem},
        {Role::Tab, UiaControlType::TabItem},
        {Role::TabPanel, UiaControlType::Pane},
        {Role::Dialog, UiaControlType::Pane},
        {Role::Menu, UiaControlType::Menu},
        {Role::MenuItem, UiaControlType::MenuItem},
        {Role::ScrollView, UiaControlType::Pane},
        {Role::Tooltip, UiaControlType::ToolTip},
        {Role::Alert, UiaControlType::Text},
        {Role::Presentation, UiaControlType::Pane},
    }};
    for (const Case& item : kCases) {
        if (!expect(control_type_for(item.role) == item.type, "control type mapping")) {
            std::cerr << "  role=" << to_string(item.role)
                      << " type=" << to_string(control_type_for(item.role)) << "\n";
            return false;
        }
    }
    return expect(!to_string(UiaControlType::Button).empty(), "control type names") &&
           expect_equal(to_string(UiaControlType::Edit), "edit", "edit name") &&
           expect_equal(to_string(UiaControlType::ToolTip), "tool tip", "tooltip name");
}

bool patterns() {
    A11yNode button = make_node(Role::Button, "Save");
    button.actions = Action::Press | Action::Focus;
    A11yNode disabled_button = button;
    disabled_button.actions = Action::Focus;

    A11yNode field = make_node(Role::TextField, "Email", "a@b");
    field.actions = Action::SetValue | Action::Focus;

    A11yNode item = make_node(Role::ListItem, "Row");
    item.states = State::Selected;

    A11yNode list = make_node(Role::List, "Rows");
    A11yNode text = make_node(Role::Text, "Paragraph");

    const UiaPatterns button_patterns = patterns_for(button);
    const UiaPatterns field_patterns = patterns_for(field);
    const UiaPatterns item_patterns = patterns_for(item);
    const UiaPatterns list_patterns = patterns_for(list);
    const UiaPatterns disabled_patterns = patterns_for(disabled_button);
    const UiaPatterns text_patterns = patterns_for(text);

    return expect(has_pattern(button_patterns, UiaPatterns::Invoke), "press -> invoke") &&
           expect(!has_pattern(button_patterns, UiaPatterns::Value), "button has no value") &&
           expect(!has_pattern(button_patterns, UiaPatterns::SelectionItem),
                  "plain button does not select") &&
           expect(has_pattern(field_patterns, UiaPatterns::Value), "set value -> value") &&
           expect(!has_pattern(field_patterns, UiaPatterns::Invoke), "field is not invokable") &&
           expect(has_pattern(item_patterns, UiaPatterns::SelectionItem),
                  "selected item -> selection item") &&
           expect(has_pattern(list_patterns, UiaPatterns::Selection), "list -> selection") &&
           expect(!has_pattern(disabled_patterns, UiaPatterns::Invoke),
                  "disabled press is not advertised") &&
           expect(text_patterns == UiaPatterns::None, "plain text advertises no pattern");
}

bool pattern_ids() {
    return expect_eq(pattern_id(UiaPatterns::Invoke), kUiaInvokePatternId, "invoke id") &&
           expect_eq(pattern_id(UiaPatterns::Value), kUiaValuePatternId, "value id") &&
           expect_eq(pattern_id(UiaPatterns::SelectionItem), kUiaSelectionItemPatternId,
                     "selection item id") &&
           expect_eq(pattern_id(UiaPatterns::Selection), kUiaSelectionPatternId, "selection id") &&
           expect_eq(pattern_id(UiaPatterns::None), 0, "no pattern has no id") &&
           expect_eq(pattern_id(UiaPatterns::Invoke | UiaPatterns::Value), 0,
                     "a multi-bit mask has no single id");
}

bool bit_operations() {
    const UiaPatterns both = UiaPatterns::Invoke | UiaPatterns::Value;
    return expect(has_pattern(both, UiaPatterns::Invoke), "or keeps the left bit") &&
           expect(has_pattern(both, UiaPatterns::Value), "or keeps the right bit") &&
           expect(!has_pattern(both, UiaPatterns::Selection), "or adds nothing else") &&
           expect((both & UiaPatterns::Value) == UiaPatterns::Value, "and selects one bit");
}

} // namespace

int main() {
    if (!control_types()) {
        return 1;
    }
    if (!patterns()) {
        return 2;
    }
    if (!pattern_ids()) {
        return 3;
    }
    if (!bit_operations()) {
        return 4;
    }
    std::cout << "a11y_uia_mapping: ok\n";
    return 0;
}
