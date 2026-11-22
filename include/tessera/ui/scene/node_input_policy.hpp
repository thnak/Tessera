// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node_types.hpp"
#include "tessera/ui/style/cursor_value.hpp"

#include <cstdint>

// What a scene node permits the input and focus systems to do with it: whether it takes part in
// painting and hit testing, whether it can hold focus, the text-selection policy and the cursor the
// app reports (docs/architecture/god-type-audit-v1.md section 3.1). Four bytes, read by the
// whole-scene passes at every node (`rendered`, `pointer_events`), so it sits on the node's first
// cache line.
//
// The type owns the values and nothing else. Each setter returns whether it changed the stored
// value, and the owner decides which dirty domain that implies: `ui::Node` keeps the frame policy
// (a visibility change reflows, a cursor change paints nothing), so a change to this record cannot
// silently move layout.
namespace tessera::ui {

class NodeInputPolicy {
public:
    // The programmatic/structural gate (a conditional branch, a recycled virtual item).
    [[nodiscard]] bool visible() const noexcept {
        return test(kVisible);
    }
    bool set_visible(bool on) noexcept {
        return assign(kVisible, on);
    }
    // CSS `visibility: hidden|collapse` (stylesheet-v1 section 11, issue #213). A separate bit from
    // `visible()` because the two have different owners and the stylesheet value must be able to
    // flip back without resurrecting a node the program hid.
    [[nodiscard]] bool style_hidden() const noexcept {
        return test(kStyleHidden);
    }
    bool set_style_hidden(bool on) noexcept {
        return assign(kStyleHidden, on);
    }
    // True when the node takes part in painting, hit testing and the accessibility tree.
    [[nodiscard]] bool rendered() const noexcept {
        return (flags_ & (kVisible | kStyleHidden)) == kVisible;
    }
    // CSS `pointer-events: auto|none` (stylesheet-v1 section 11, issue #247).
    [[nodiscard]] bool pointer_events() const noexcept {
        return test(kPointerEvents);
    }
    bool set_pointer_events(bool on) noexcept {
        return assign(kPointerEvents, on);
    }

    [[nodiscard]] bool focusable() const noexcept {
        return test(kFocusable);
    }
    bool set_focusable(bool on) noexcept {
        return assign(kFocusable, on);
    }
    [[nodiscard]] bool auto_focus() const noexcept {
        return test(kAutoFocus);
    }
    bool set_auto_focus(bool on) noexcept {
        return assign(kAutoFocus, on);
    }
    // JSX `focusTrap` / `typeahead` / `useInteractiveRole` (issues #236, #635).
    [[nodiscard]] bool focus_trap() const noexcept {
        return test(kFocusTrap);
    }
    bool set_focus_trap(bool on) noexcept {
        return assign(kFocusTrap, on);
    }
    [[nodiscard]] bool typeahead() const noexcept {
        return test(kTypeahead);
    }
    bool set_typeahead(bool on) noexcept {
        return assign(kTypeahead, on);
    }
    [[nodiscard]] bool interactive_parent() const noexcept {
        return test(kInteractiveParent);
    }
    bool set_interactive_parent(bool on) noexcept {
        return assign(kInteractiveParent, on);
    }

    [[nodiscard]] UserSelect user_select() const noexcept {
        return user_select_;
    }
    bool set_user_select(UserSelect value) noexcept {
        return exchange(user_select_, value);
    }
    [[nodiscard]] style::CursorKind cursor() const noexcept {
        return cursor_;
    }
    bool set_cursor(style::CursorKind value) noexcept {
        return exchange(cursor_, value);
    }
    [[nodiscard]] FocusGroup focus_group() const noexcept {
        return focus_group_;
    }
    bool set_focus_group(FocusGroup value) noexcept {
        return exchange(focus_group_, value);
    }

private:
    enum Bit : std::uint8_t {
        kVisible = 1u << 0,
        kFocusable = 1u << 1,
        kAutoFocus = 1u << 2,
        kFocusTrap = 1u << 3,
        kTypeahead = 1u << 4,
        kPointerEvents = 1u << 5,
        kStyleHidden = 1u << 6,
        kInteractiveParent = 1u << 7,
    };
    [[nodiscard]] bool test(Bit bit) const noexcept {
        return (flags_ & bit) != 0;
    }
    bool assign(Bit bit, bool on) noexcept {
        const auto next = static_cast<std::uint8_t>(on ? (flags_ | bit) : (flags_ & ~bit));
        const bool changed = next != flags_;
        flags_ = next;
        return changed;
    }
    template<class T> static bool exchange(T& slot, T value) noexcept {
        const bool changed = slot != value;
        slot = value;
        return changed;
    }

    // Defaults: visible, `pointer-events: auto`, everything else off.
    std::uint8_t flags_ = kVisible | kPointerEvents;
    UserSelect user_select_ = UserSelect::Auto;           // resolved `user-select` value
    style::CursorKind cursor_ = style::CursorKind::Auto;  // the initial `auto` is the platform arrow
    FocusGroup focus_group_ = FocusGroup::None;
};

static_assert(sizeof(NodeInputPolicy) == 4, "NodeInputPolicy is four bytes on the first line");

} // namespace tessera::ui
