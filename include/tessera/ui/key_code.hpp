// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

namespace tessera::ui::key {

// Logical keys carried in InputEvent::key. Printable letters and digits use the upper-case ASCII
// code ('A'..'Z', '0'..'9'), which is the key's logical meaning in the active layout, not its
// physical position. Characters typed (including punctuation, other scripts and dead-key results)
// arrive as Text events, never as key codes. Values at or above 0x100 are non-printable keys.
inline constexpr KeyCode kUnknown = 0;
inline constexpr KeyCode kBackspace = 0x100;
inline constexpr KeyCode kTab = 0x101;
inline constexpr KeyCode kEnter = 0x102;
inline constexpr KeyCode kEscape = 0x103;
inline constexpr KeyCode kSpace = 0x104;
inline constexpr KeyCode kInsert = 0x105;
inline constexpr KeyCode kDelete = 0x106;
inline constexpr KeyCode kHome = 0x107;
inline constexpr KeyCode kEnd = 0x108;
inline constexpr KeyCode kPageUp = 0x109;
inline constexpr KeyCode kPageDown = 0x10A;
inline constexpr KeyCode kArrowLeft = 0x10B;
inline constexpr KeyCode kArrowUp = 0x10C;
inline constexpr KeyCode kArrowRight = 0x10D;
inline constexpr KeyCode kArrowDown = 0x10E;
inline constexpr KeyCode kShift = 0x10F;
inline constexpr KeyCode kControl = 0x110;
inline constexpr KeyCode kAlt = 0x111;
inline constexpr KeyCode kMeta = 0x112;
inline constexpr KeyCode kCapsLock = 0x113;
// F1..F12 are kF1 + 0..11.
inline constexpr KeyCode kF1 = 0x120;

} // namespace tessera::ui::key
