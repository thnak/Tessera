// SPDX-License-Identifier: MIT
//
// UTF-8 to UTF-16 for the Android clipboard bridge (issue #189, docs/guides/android-lane.md
// section 3.16).
//
// `hal::Clipboard` speaks UTF-8; Android's `ClipboardManager` speaks `java.lang.String`, which is
// UTF-16. The one rule that matters is that a code point above U+FFFF becomes a SURROGATE PAIR, two
// UTF-16 units. A naive per-byte loop emits one unit per code point, which turns every emoji into a
// mojibake box and makes every offset after it wrong.
//
// The rules are pure over plain bytes, so they run on every lane with no NDK, no JNI and no device.

#include "android_clipboard_text.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace android = tessera::hal::android;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

} // namespace

int main() {
    // --- ASCII is one unit per byte ----------------------------------------------------------
    {
        const std::u16string units = android::utf8_to_utf16("hello");
        check(units.size() == 5, "ASCII is one UTF-16 unit per byte");
        check(units == u"hello", "ASCII round-trips");
        check(android::utf16_length_from_utf8("hello") == 5, "ASCII length is the byte count");
    }

    // --- An empty string is empty ------------------------------------------------------------
    {
        check(android::utf8_to_utf16("").empty(), "an empty string converts to empty");
        check(android::utf16_length_from_utf8("") == 0, "an empty string has zero units");
        check(android::utf16_to_utf8(u"").empty(), "an empty UTF-16 string converts to empty");
    }

    // --- Two-byte UTF-8 is one unit ----------------------------------------------------------
    //
    // U+00E9 (e-acute) is 2 UTF-8 bytes and 1 UTF-16 unit. A loop that counted bytes would report
    // two units and put every offset after it one too far.
    {
        const std::u16string units = android::utf8_to_utf16("\xC3\xA9");
        check(units.size() == 1, "a two-byte code point is one UTF-16 unit");
        check(units[0] == 0x00E9, "the code point survives");
        check(android::utf16_length_from_utf8("\xC3\xA9") == 1, "the length agrees");
    }

    // --- Three-byte UTF-8 is one unit --------------------------------------------------------
    //
    // U+4E16 (the CJK character for "world") is 3 UTF-8 bytes and 1 UTF-16 unit.
    {
        const std::u16string units = android::utf8_to_utf16("\xE4\xB8\x96");
        check(units.size() == 1, "a three-byte code point is one UTF-16 unit");
        check(units[0] == 0x4E16, "the CJK code point survives");
    }

    // --- Four-byte UTF-8 is a SURROGATE PAIR --------------------------------------------------
    //
    // U+1F600 (grinning face) is 4 UTF-8 bytes and 2 UTF-16 units. This is the rule that breaks a
    // naive implementation: one unit per code point turns the emoji into a mojibake box.
    {
        const std::u16string units = android::utf8_to_utf16("\xF0\x9F\x98\x80");
        check(units.size() == 2, "a four-byte code point is two UTF-16 units");
        check(units[0] == 0xD83DU, "the high surrogate is correct");
        check(units[1] == 0xDE00U, "the low surrogate is correct");
        check(android::utf16_length_from_utf8("\xF0\x9F\x98\x80") == 2,
              "the length counts the pair as two");
    }

    // --- A round trip preserves every code point ---------------------------------------------
    {
        const std::string original = "a\xC3\xA9\xE4\xB8\x96\xF0\x9F\x98\x80z";
        const std::u16string units = android::utf8_to_utf16(original);
        // a(1) + e-acute(1) + CJK(1) + emoji(2) + z(1) = 6
        check(units.size() == 6, "the mixed string is six units");
        check(android::utf16_to_utf8(units) == original, "the round trip preserves the bytes");
    }

    // --- Invalid UTF-8 becomes U+FFFD, never a dropped byte ----------------------------------
    //
    // Dropping the byte would make the UTF-16 length disagree with the UTF-8 length, and every
    // offset after it would be wrong.
    {
        const std::u16string units = android::utf8_to_utf16("\xFF");
        check(units.size() == 1, "an invalid byte becomes one unit");
        check(units[0] == 0xFFFDU, "an invalid byte becomes U+FFFD");
        check(android::utf16_length_from_utf8("\xFF") == 1, "the length counts it");
    }

    // --- A truncated sequence becomes U+FFFD -------------------------------------------------
    {
        // The first two bytes of a three-byte sequence, with the third missing.
        const std::u16string units = android::utf8_to_utf16("\xE4\xB8");
        check(units.size() == 2, "a truncated sequence becomes one unit per byte");
        check(units[0] == 0xFFFDU && units[1] == 0xFFFDU, "both bytes become U+FFFD");
    }

    // --- An overlong encoding is rejected ----------------------------------------------------
    //
    // 0xC0 0x80 is the overlong form of NUL. Accepting it would let a caller smuggle a NUL past a
    // length check.
    {
        const std::u16string units = android::utf8_to_utf16("\xC0\x80");
        check(units.size() == 2, "an overlong sequence becomes one unit per byte");
        check(units[0] == 0xFFFDU, "an overlong sequence is rejected");
    }

    // --- A lone surrogate in UTF-16 becomes U+FFFD -------------------------------------------
    //
    // The same rule the IME path applies, so the two directions agree on malformed input.
    {
        const std::u16string lone_high(1, static_cast<char16_t>(0xD83DU));
        check(android::utf16_to_utf8(lone_high) == "\xEF\xBF\xBD",
              "a lone high surrogate becomes U+FFFD");
        const std::u16string lone_low(1, static_cast<char16_t>(0xDE00U));
        check(android::utf16_to_utf8(lone_low) == "\xEF\xBF\xBD",
              "a lone low surrogate becomes U+FFFD");
    }

    // --- A surrogate pair in UTF-16 becomes four UTF-8 bytes ---------------------------------
    {
        const std::u16string pair = {static_cast<char16_t>(0xD83DU),
                                     static_cast<char16_t>(0xDE00U)};
        check(android::utf16_to_utf8(pair) == "\xF0\x9F\x98\x80",
              "a surrogate pair becomes four UTF-8 bytes");
    }

    // --- A high surrogate followed by a non-low unit is two replacements ---------------------
    {
        const std::u16string mixed = {static_cast<char16_t>(0xD83DU), u'a'};
        const std::string converted = android::utf16_to_utf8(mixed);
        check(converted == "\xEF\xBF\xBD"
                           "a",
              "a high surrogate followed by a BMP unit is a replacement then the unit");
    }

    if (failures != 0) {
        std::cerr << "android_clipboard_text: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_clipboard_text: ok\n";
    return 0;
}
