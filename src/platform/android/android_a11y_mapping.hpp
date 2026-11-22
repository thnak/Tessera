// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"

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

// The Android accessibility action bits (`AccessibilityNodeInfo.AccessibilityAction` ids). The
// engine's `AccessibilityAction` is a bit set; the platform's is a list of action ids. The mapping
// turns one into the other.
enum class A11yAction : std::int32_t {
    None = 0,
    Click = 1,
    LongClick = 2,
    Focus = 4,
    ClearFocus = 8,
    ScrollForward = 16,
    ScrollBackward = 32,
    SetText = 64,
    SetSelection = 128,
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
    // The action bits the node supports, as `A11yAction`.
    std::int32_t actions = 0;
};

// The platform class name for a role, or nullptr for a role the platform should not announce as
// itself. A `Presentation` node is decorative: the node is not emitted, its children are.
[[nodiscard]] const char* class_name_for(ui::a11y::Role role) noexcept;

// The platform's action bits for an engine action set.
[[nodiscard]] std::int32_t actions_for(ui::a11y::Action actions) noexcept;

// One node's platform description. This is the whole mapping: the Java side creates an
// `AccessibilityNodeInfo`, sets the fields from this struct, and adds the children.
[[nodiscard]] A11yNodeInfo describe(const ui::a11y::A11yNode& node) noexcept;

// True when the platform should emit this node at all. A decorative node is skipped but its
// children are not, which is what `Role::Presentation` means.
[[nodiscard]] bool emits_node(ui::a11y::Role role) noexcept;

} // namespace tessera::hal::android
