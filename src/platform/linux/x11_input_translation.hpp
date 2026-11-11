// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <cstdint>

// Pure mapping rules for X11 core input. This header deliberately does not include <X11/Xlib.h>:
// keysyms, button numbers and state bits are stable protocol values, so the rules are unit-testable
// on any platform without a display. The Xlib-facing translator (x11_input.cpp) calls into these.

namespace tessera::hal::x11 {

// Core-protocol modifier state bits (X.h ShiftMask..Mod5Mask). Mod1 is Alt, Mod2 is NumLock and
// Mod4 is Super on every mainstream keymap.
inline constexpr unsigned kShiftMask = 1U << 0;
inline constexpr unsigned kLockMask = 1U << 1;
inline constexpr unsigned kControlMask = 1U << 2;
inline constexpr unsigned kMod1Mask = 1U << 3;
inline constexpr unsigned kMod2Mask = 1U << 4;
inline constexpr unsigned kMod4Mask = 1U << 6;

[[nodiscard]] ui::Modifier modifiers_from_state(unsigned state);

// Layout-level key for a keysym (the keysym with no modifier applied). Letters and digits map to
// upper-case ASCII, matching ui/key_code.hpp; unmapped keysyms yield kUnknown.
[[nodiscard]] ui::KeyCode logical_key_from_keysym(unsigned long keysym);

// Unicode scalar a keysym types, or 0 when it types nothing (navigation, function and modifier
// keys, and control characters that arrive as key events instead). Covers ASCII, Latin-1, the
// keypad digits and the 0x01000000-offset Unicode keysyms; full composition (dead keys, XIM) is
// outside this core path.
[[nodiscard]] std::uint32_t codepoint_from_keysym(unsigned long keysym);

// What one core button press means. X11 reports wheel notches as button 4..7 presses.
struct ButtonAction {
    enum class Kind : std::uint8_t { Ignored, Button, Wheel };
    Kind kind = Kind::Ignored;
    ui::PointerButton button{}; // default-initialized: X.h defines a `None` macro
    // Wheel notches with the web sign (positive dy scrolls content down, positive dx right).
    float dx = 0.0F;
    float dy = 0.0F;
};

[[nodiscard]] ButtonAction button_action(unsigned x_button);

} // namespace tessera::hal::x11
