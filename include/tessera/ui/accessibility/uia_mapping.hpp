// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"

#include <cstdint>
#include <string_view>

namespace tessera::ui::a11y {

// Windows UI Automation identifiers (docs/specs/accessibility-v1.md section 7,
// docs/design/accessibility.md section 3). The numeric values are the Windows SDK
// `UIAutomationClient.h` ids, repeated here as plain integers so this layer's headers compile on
// every platform without the Windows SDK. `uia_sdk_probe.cpp` static_asserts them against the SDK
// on Windows, so a drift is a compile error instead of a runtime mismatch.
inline constexpr std::int32_t kUiaBoundingRectanglePropertyId = 30001;
inline constexpr std::int32_t kUiaControlTypePropertyId = 30003;
inline constexpr std::int32_t kUiaNamePropertyId = 30005;
inline constexpr std::int32_t kUiaHasKeyboardFocusPropertyId = 30008;
inline constexpr std::int32_t kUiaIsEnabledPropertyId = 30010;
inline constexpr std::int32_t kUiaAutomationIdPropertyId = 30011;
inline constexpr std::int32_t kUiaHelpTextPropertyId = 30013;
inline constexpr std::int32_t kUiaIsOffscreenPropertyId = 30022;
inline constexpr std::int32_t kUiaFrameworkIdPropertyId = 30024;
inline constexpr std::int32_t kUiaValueValuePropertyId = 30045;
inline constexpr std::int32_t kUiaValueIsReadOnlyPropertyId = 30046;
inline constexpr std::int32_t kUiaSelectionItemIsSelectedPropertyId = 30079;

// Pattern ids passed to `IRawElementProviderSimple::GetPatternProvider`. Only the patterns the
// semantics snapshot can answer are advertised: Text and Scroll need data the snapshot does not
// carry yet and are listed as deferred in the spec.
inline constexpr std::int32_t kUiaInvokePatternId = 10000;
inline constexpr std::int32_t kUiaSelectionPatternId = 10001;
inline constexpr std::int32_t kUiaValuePatternId = 10002;
inline constexpr std::int32_t kUiaSelectionItemPatternId = 10010;

// `UIA_FrameworkIdPropertyId` value; the toolkit identity the provider reports.
inline constexpr std::string_view kUiaFrameworkId = "Tessera";

// The UIA control type a semantics role is exposed as. Values match `UIAutomationClient.h`.
enum class UiaControlType : std::int32_t {
    Button = 50000,
    CheckBox = 50002,
    ComboBox = 50003,
    Edit = 50004,
    Hyperlink = 50005,
    Image = 50006,
    ListItem = 50007,
    List = 50008,
    Menu = 50009,
    MenuItem = 50011,
    ProgressBar = 50012,
    RadioButton = 50013,
    Slider = 50015,
    TabItem = 50019,
    Text = 50020,
    ToolTip = 50022,
    Group = 50026,
    Document = 50030,
    Window = 50032,
    Pane = 50033,
};

// Pattern bit set the provider surface reports for one semantics node. The bits are this layer's
// own flags (not UIA ids); `pattern_id` maps one bit to the id a client passes to
// `GetPatternProvider`.
enum class UiaPatterns : std::uint32_t {
    None = 0,
    Invoke = 1U << 0,        // `IInvokeProvider`; derived from `Action::Press`
    Value = 1U << 1,         // `IValueProvider`; derived from `Action::SetValue`
    SelectionItem = 1U << 2, // `ISelectionItemProvider`; selectable roles and `State::Selected`
    Selection = 1U << 3,     // `ISelectionProvider`; list/menu containers
};

constexpr UiaPatterns operator|(UiaPatterns lhs, UiaPatterns rhs) {
    return static_cast<UiaPatterns>(static_cast<std::uint32_t>(lhs) |
                                    static_cast<std::uint32_t>(rhs));
}
constexpr UiaPatterns operator&(UiaPatterns lhs, UiaPatterns rhs) {
    return static_cast<UiaPatterns>(static_cast<std::uint32_t>(lhs) &
                                    static_cast<std::uint32_t>(rhs));
}
constexpr bool has_pattern(UiaPatterns value, UiaPatterns flag) {
    return (value & flag) != UiaPatterns::None;
}

// Stable lowercase name (tests, diagnostics, and the COM adapter's debug paths).
[[nodiscard]] std::string_view to_string(UiaControlType type);

// Role -> control type. Container roles (generic, scroll view, dialog, tab panel) map to Pane;
// switch maps to CheckBox because UIA has no switch control type; alert maps to Text so its name is
// read like a live region.
[[nodiscard]] UiaControlType control_type_for(Role role);

// Patterns one snapshot node supports, from its role, states, and actions.
[[nodiscard]] UiaPatterns patterns_for(const A11yNode& node);

// UIA pattern id for a single-bit mask; 0 for `None` or a multi-bit mask.
[[nodiscard]] std::int32_t pattern_id(UiaPatterns pattern);

} // namespace tessera::ui::a11y
