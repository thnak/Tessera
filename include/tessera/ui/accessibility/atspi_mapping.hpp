// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace tessera::ui::a11y {

// AT-SPI2 mapping for the semantics snapshot (docs/specs/accessibility-v1.md section 7,
// docs/design/accessibility.md section 3). Pure, allocation-free translation from `A11yNode` to the
// values the AT-SPI2 interfaces return: role enum/name, the 64-bit state set, the interface list,
// action names, text content, and the numeric value.
//
// Nothing here touches a bus or a scene: the bridge dispatcher reads these functions, which keeps
// the mapping table reviewable on its own and testable without a daemon.

// `AtspiRole` values from at-spi2-core; only the roles this bridge emits are listed.
enum class AtspiRole : std::uint32_t {
    Invalid = 0,
    Alert = 2,
    CheckBox = 7,
    Dialog = 16,
    Filler = 20,
    Frame = 22,
    Image = 26,
    Label = 28,
    List = 30,
    ListItem = 31,
    Menu = 32,
    MenuItem = 34,
    PageTab = 36,
    PageTabList = 37,
    Panel = 38,
    ProgressBar = 41,
    PushButton = 42,
    RadioButton = 43,
    ScrollPane = 48,
    Slider = 50,
    Text = 60,
    ToggleButton = 61,
    ToolTip = 63,
    Unknown = 66,
    Window = 68,
    Application = 73,
    Entry = 76,
    Heading = 80,
    Section = 82,
    Link = 85,
};

[[nodiscard]] AtspiRole atspi_role(Role role);
// Localized-name stand-in returned by `GetRoleName` (English, matching at-spi2-core's catalog).
[[nodiscard]] std::string_view atspi_role_name(Role role);

// `AtspiStateType` bit indices. The state set is two uint32 words, low = indices 0..31.
enum class AtspiState : std::uint32_t {
    Active = 1,
    Armed = 2,
    Busy = 3,
    Checked = 4,
    Collapsed = 5,
    Defunct = 6,
    Editable = 7,
    Enabled = 9,
    Expandable = 10,
    Expanded = 11,
    Focusable = 12,
    Focused = 13,
    Multiselectable = 19,
    Pressed = 21,
    Selectable = 23,
    Selected = 24,
    Sensitive = 25,
    Showing = 26,
    Vertical = 30,
    Visible = 31,
    Checkable = 41,
};

struct StateSet {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
};

// Visible, enabled/focusable/checked/expanded/selected bits derived from `A11yNode::states`.
// Emitted nodes are always Showing and Visible by construction (the snapshot skips hidden
// subtrees).
[[nodiscard]] StateSet atspi_states(const A11yNode& node);
[[nodiscard]] bool has_state(const StateSet& states, AtspiState state);

// Interface names, shared by `GetInterfaces` and the tests.
inline constexpr std::string_view kAccessibleInterface = "org.a11y.atspi.Accessible";
inline constexpr std::string_view kActionInterface = "org.a11y.atspi.Action";
inline constexpr std::string_view kApplicationInterface = "org.a11y.atspi.Application";
inline constexpr std::string_view kComponentInterface = "org.a11y.atspi.Component";
inline constexpr std::string_view kTextInterface = "org.a11y.atspi.Text";
inline constexpr std::string_view kValueInterface = "org.a11y.atspi.Value";
// The standard property interface; the bridge serves `Get`/`GetAll` from a fixed property table.
inline constexpr std::string_view kPropertiesInterface = "org.freedesktop.DBus.Properties";

struct InterfaceSet {
    bool accessible = true; // always, for every emitted node
    bool action = false;
    bool application = false; // the root object only
    bool component = true;    // every node has bounds
    bool text = false;
    bool value = false;
};

[[nodiscard]] InterfaceSet atspi_interfaces(const A11yNode& node, bool is_root);
// Writes the interface names of `interfaces` into `out` in a stable order; returns the count.
// `out` must have room for 6 entries.
[[nodiscard]] std::size_t atspi_interface_names(const InterfaceSet& interfaces,
                                                std::span<std::string_view> out);

// Action names: `("click", "", "")` for pressable roles, none otherwise. AT-SPI2 exposes three
// strings per action (name, description, key binding).
inline constexpr std::uint32_t kMaxActionNames = 1;
[[nodiscard]] std::uint32_t atspi_action_count(const A11yNode& node);
[[nodiscard]] std::string_view atspi_action_name(const A11yNode& node, std::uint32_t index);

// Text content: a text field exposes its value, everything else its name.
[[nodiscard]] std::string_view atspi_text(const A11yNode& node);

// Numeric value for the Value interface. The snapshot carries one value string; sliders and
// progress bars report it as the current value against a fixed 0..100 range with a 1 increment
// (ARIA valuemin/valuemax are not in the snapshot yet, see docs/specs/accessibility-v1.md section
// 12). Every other role reports `has_value == false`.
struct AtspiValue {
    bool has_value = false;
    double current = 0.0;
    double minimum = 0.0;
    double maximum = 100.0;
    double increment = 1.0;
};

[[nodiscard]] AtspiValue atspi_value(const A11yNode& node);

// Component layer: AT-SPI2 layers (`AtspiComponentLayer`).
enum class AtspiLayer : std::uint32_t {
    Invalid = 0,
    Background = 1,
    Canvas = 2,
    Widget = 3,
    Document = 4,
    Popup = 6,
    Overlay = 7,
    Window = 8
};

// Filler/panel/section nodes sit behind their widgets; interactive roles are widgets; dialogs and
// menus are popups, which screen readers use for modality decisions.
[[nodiscard]] AtspiLayer atspi_layer(const A11yNode& node);

// Object path for a snapshot index: "/org/a11y/atspi/accessible/<index>". Index 0 is the root and
// also carries the Application interface. Returns the number of bytes written (excluding the NUL),
// or 0 when the buffer is too small.
inline constexpr std::string_view kAccessiblePathPrefix = "/org/a11y/atspi/accessible/";
// Object path of snapshot index 0, the application root.
inline constexpr std::string_view kRootObjectPath = "/org/a11y/atspi/accessible/0";
// Reported as the root's parent / as a null reference: an empty bus name means "no object".
inline constexpr std::string_view kNullObjectPath = "/org/a11y/atspi/accessible/null";
[[nodiscard]] std::size_t format_object_path(A11yIndex index, std::span<char> out);
// Parses the index out of a path produced by `format_object_path`; kInvalidIndex when the path is
// not one of ours (including a trailing segment, which would be a child path of another node).
[[nodiscard]] A11yIndex parse_object_path(std::string_view path);

} // namespace tessera::ui::a11y
