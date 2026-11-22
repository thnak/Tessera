// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"
#include "tessera/ui/accessibility/snapshot_provider.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// The Android accessibility vocabulary (issue #189, docs/design/mobile-integration.md section 3.4).
//
// This header deliberately does not include any Android or JNI header: the values below are the
// platform's own constants, restated so the mapping is unit-testable on every lane. The Java
// `AccessibilityNodeProvider` reads them across JNI.
//
// Why the values are restated rather than included: the same reason `android_input_translation.hpp`
// restates the action constants. A test lane with no Android SDK must still be able to compile and
// exercise the mapping, and the platform's class-name string for a control is a stable API value
// (`android.widget.Button` and friends have not changed).
//
// This is the Android column of the accessibility role table. It is a separate file rather than
// another column in `a11y_role_traits.hpp` because that table's every row would have to grow, and a
// row that is missed there produces a silently generic answer. Here a missing case is a compile
// error because the switch has no `default`.

namespace tessera::hal::android {

// The Android accessibility action ids (`AccessibilityNodeInfo.ACTION_*`). The engine's `Action` is
// a bit set; the platform's is a set of these ids, added to an `AccessibilityNodeInfo` one at a
// time. The mapping turns one into the other.
//
// These are NOT consecutive powers of two, and they are not ours to choose: they are the platform's
// wire values, and a screen reader sends back exactly the id it was offered. `ACTION_CLICK` is 16
// and `ACTION_FOCUS` is 1, so numbering them in enum order would make a click arrive as a focus
// request. The values below were read from `android.jar` with `javap -constants`.
enum class A11yAction : std::int32_t {
    None = 0,
    Focus = 1,                     // ACTION_FOCUS
    ClearFocus = 2,                // ACTION_CLEAR_FOCUS
    Select = 4,                    // ACTION_SELECT
    ClearSelection = 8,            // ACTION_CLEAR_SELECTION
    Click = 16,                    // ACTION_CLICK
    LongClick = 32,                // ACTION_LONG_CLICK
    AccessibilityFocus = 64,       // ACTION_ACCESSIBILITY_FOCUS
    ClearAccessibilityFocus = 128, // ACTION_CLEAR_ACCESSIBILITY_FOCUS
    ScrollForward = 4096,          // ACTION_SCROLL_FORWARD
    ScrollBackward = 8192,         // ACTION_SCROLL_BACKWARD
    SetSelection = 131072,         // ACTION_SET_SELECTION
    SetText = 2097152,             // ACTION_SET_TEXT
};

// At most this many action ids for one node. The engine has four actions and two of them map to a
// pair of platform ids, so eight is the ceiling; the array is sized for the full set.
inline constexpr std::size_t kMaxActions = 8;

// The platform's action ids for an engine action set, in a fixed order.
//
// A list rather than a bit set: the platform's ids are not powers of two (`ACTION_CLICK` is 16 and
// `ACTION_SELECT` is 4), so `Click | LongClick` would be 48, which is not an id the platform knows.
// The platform API takes them one at a time anyway (`addAction`).
struct A11yActionList {
    std::array<A11yAction, kMaxActions> actions{};
    std::size_t count = 0;
    bool overflowed = false; // more ids than kMaxActions: the extras were dropped
};

// What the platform needs to describe one node. Mirrors the fields of
// `AccessibilityNodeInfo`, in the platform's own terms.
struct A11yNodeInfo {
    // The platform's class name, which is what a screen reader announces as the control kind
    // ("Button", "EditText"). A `nullptr` means the node is not announced (a decorative node).
    const char* class_name = nullptr;
    // The node's text, borrowed from the snapshot. Empty means no text.
    std::string_view text;
    // The node's content description, borrowed. Empty means none.
    std::string_view description;
    // Whether the platform should treat the node as focusable, enabled, selected, checked.
    bool focusable = false;
    bool enabled = true;
    bool selected = false;
    bool checkable = false;
    bool checked = false;
    bool editable = false;
    bool scrollable = false;
    // The platform's action ids the node supports. A list, not a bit set: see `A11yActionList`.
    A11yActionList actions;
};

// The platform class name for a role, or nullptr for a role the platform should not announce as
// itself. A `Presentation` node is decorative: the node is not emitted, its children are.
[[nodiscard]] const char* class_name_for(ui::a11y::Role role) noexcept;

[[nodiscard]] A11yActionList actions_for(ui::a11y::Action actions) noexcept;

// True when `list` contains `action`. There is no bit test, because there is no bit set.
[[nodiscard]] bool actions_contain(const A11yActionList& list, A11yAction action) noexcept;

// The engine action a platform action id means, or `Action::None` for an id the engine has no
// concept of (Select, ClearSelection are the platform's own bookkeeping).
//
// This is the direction a screen reader exercises: the platform sends back the id it was offered,
// so a mistake here is silent - the control simply does nothing. `Action::Press` covers both click
// and long click because the engine has no separate long-press dispatch, and `Action::Focus` covers
// the four focus ids for the same reason.
[[nodiscard]] ui::a11y::Action engine_action_for(std::int32_t platform_action) noexcept;

// One node's platform description. This is the whole mapping for the push-side snapshot type: the
// Java side creates an `AccessibilityNodeInfo`, sets the fields from this struct, and adds the
// children.
[[nodiscard]] A11yNodeInfo describe(const ui::a11y::A11yNode& node) noexcept;

// The same mapping over the pull-side provider's answer. Two overloads rather than one template
// because the two types genuinely differ (a snapshot node carries child indices and inline text
// arrays; a provider answer carries world bounds and a role-derived class name), and a shared
// template would have to reach into both.
[[nodiscard]] A11yNodeInfo describe_node(const ui::a11y::ProviderAnswer& answer) noexcept;

// True when the platform should emit this node at all. A decorative node is skipped but its
// children are not, which is what `Role::Presentation` means.
[[nodiscard]] bool emits_node(ui::a11y::Role role) noexcept;

} // namespace tessera::hal::android
