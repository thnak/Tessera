// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// UTF-8 to UTF-16 for the Android clipboard bridge (issue #189, docs/guides/android-lane.md
// section 3.16).
//
// `hal::Clipboard` speaks UTF-8; Android's `ClipboardManager` speaks `java.lang.String`, which is
// UTF-16. The IME path already converts the other direction (`utf8_step`,
// `utf8_offset_from_utf16` in android_ime_translation.hpp); this is the reverse, and it is PURE so
// the surrogate handling is unit-tested on every lane instead of only on a phone.
//
// The one rule that matters: a code point above U+FFFF becomes a SURROGATE PAIR, two UTF-16 units.
// A naive per-byte loop emits one unit per code point, which turns every emoji into a mojibake box
// and makes every offset after it wrong. A lone surrogate in the input is encoded as U+FFFD, the
// same choice the IME path makes, so the two directions agree.

namespace tessera::hal::android {

// The UTF-16 length of a UTF-8 string, in code units. A code point above U+FFFF counts as two.
// Invalid UTF-8 bytes count as one unit each (they become U+FFFD), so the result is always a
// truthful upper bound for `utf8_to_utf16`.
[[nodiscard]] std::size_t utf16_length_from_utf8(std::string_view text) noexcept;

// Converts UTF-8 to UTF-16. Invalid sequences become U+FFFD rather than being dropped, so the
// result always has the length `utf16_length_from_utf8` reports.
[[nodiscard]] std::u16string utf8_to_utf16(std::string_view text);

// Converts UTF-16 to UTF-8. A lone surrogate becomes U+FFFD, matching `utf8_step`'s rule in the IME
// path, so a round trip through both directions is stable.
[[nodiscard]] std::string utf16_to_utf8(std::u16string_view text);

} // namespace tessera::hal::android
