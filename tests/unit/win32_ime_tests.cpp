// SPDX-License-Identifier: MIT

#include "win32_ime_translation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

using tessera::hal::win32::composition_event;
using tessera::hal::win32::ImeStrings;
using tessera::hal::win32::kMaxImeStringUnits;
using tessera::hal::win32::result_event;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::kMaxEditTextBytes;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "win32_ime: " << message << "\n";
    }
    return condition;
}

std::string_view as_bytes(const InputEvent& event) {
    return std::string_view(event.text.data(), event.text_size);
}

} // namespace

int main() {
    // GCS_RESULTSTR commits multi-codepoint text; the payload is UTF-8 and the offsets are UTF-16
    // code units.
    ImeStrings result;
    result.has_result = true;
    result.result[0] = u'a';
    result.result[1] = u'b';
    result.result_length = 2;
    InputEvent event;
    if (!expect(result_event(result, event), "ascii result translates") ||
        !expect(event.type == InputEventType::EditText, "result is EditText") ||
        !expect(as_bytes(event) == "ab", "ascii result UTF-8") ||
        !expect(event.text_truncated == false, "short result is not truncated") ||
        !expect(event.selection_start == 0 && event.selection_end == 0,
                "committed text has no selection")) {
        return 1;
    }

    // Surrogate pairs combine into one 4-byte UTF-8 sequence.
    ImeStrings emoji;
    emoji.has_result = true;
    emoji.result[0] = static_cast<char16_t>(0xD83D);
    emoji.result[1] = static_cast<char16_t>(0xDE00);
    emoji.result_length = 2;
    if (!expect(result_event(emoji, event), "surrogate pair result translates") ||
        !expect(event.text_size == 4, "surrogate pair is four UTF-8 bytes") ||
        !expect(static_cast<unsigned char>(event.text[0]) == 0xF0, "emoji first byte") ||
        !expect(static_cast<unsigned char>(event.text[3]) == 0x80, "emoji last byte")) {
        return 2;
    }

    // A lone surrogate half becomes U+FFFD instead of invalid UTF-8.
    ImeStrings lone;
    lone.has_result = true;
    lone.result[0] = static_cast<char16_t>(0xD83D);
    lone.result_length = 1;
    if (!expect(result_event(lone, event), "lone surrogate translates") ||
        !expect(event.text_size == 3, "replacement character is three bytes") ||
        !expect(static_cast<unsigned char>(event.text[0]) == 0xEF, "replacement first byte") ||
        !expect(static_cast<unsigned char>(event.text[1]) == 0xBF &&
                    static_cast<unsigned char>(event.text[2]) == 0xBD,
                "replacement bytes")) {
        return 3;
    }

    // No or empty result means nothing was committed.
    ImeStrings empty_result;
    empty_result.has_result = true;
    empty_result.result_length = 0;
    if (!expect(!result_event(empty_result, event), "empty result is not committed") ||
        !expect(!result_event(ImeStrings{}, event), "absent result is not committed")) {
        return 4;
    }

    // GCS_COMPSTR produces a CompositionUpdate with the cursor as a clamped UTF-16 range.
    ImeStrings composition;
    composition.has_composition = true;
    composition.composition[0] = u'x';
    composition.composition[1] = u'y';
    composition.composition[2] = u'z';
    composition.composition_length = 3;
    composition.cursor = 2;
    if (!expect(composition_event(composition, event), "composition translates") ||
        !expect(event.type == InputEventType::CompositionUpdate, "composition event type") ||
        !expect(as_bytes(event) == "xyz", "composition UTF-8") ||
        !expect(event.selection_start == 2 && event.selection_end == 2,
                "cursor maps to a clamped UTF-16 range")) {
        return 5;
    }

    // Empty composition strings are valid updates (they clear the in-progress text).
    ImeStrings empty_composition;
    empty_composition.has_composition = true;
    empty_composition.cursor = 9;
    if (!expect(composition_event(empty_composition, event), "empty composition update") ||
        !expect(event.text_size == 0, "empty composition has no bytes") ||
        !expect(event.selection_start == 0 && event.selection_end == 0,
                "empty composition clamps the cursor")) {
        return 6;
    }

    // A cursor past the composition length is clamped to the length.
    composition.cursor = 99;
    if (!expect(composition_event(composition, event), "composition cursor clamp") ||
        !expect(event.selection_start == 3 && event.selection_end == 3,
                "cursor clamps to composition length")) {
        return 7;
    }

    // Payloads past the inline capacity are truncated at a UTF-8 boundary and flagged.
    ImeStrings long_composition;
    long_composition.has_composition = true;
    long_composition.composition_length = 300;
    for (std::size_t i = 0; i < long_composition.composition_length; ++i) {
        long_composition.composition[i] = u'a';
    }
    if (!expect(composition_event(long_composition, event), "long composition translates") ||
        !expect(event.text_size == kMaxEditTextBytes, "long composition fills the inline buffer") ||
        !expect(event.text_truncated, "long composition reports truncation") ||
        !expect(long_composition.composition_length <= kMaxImeStringUnits,
                "test stays inside the platform read bound")) {
        return 8;
    }

    ImeStrings boundary;
    boundary.has_composition = true;
    boundary.composition_length = kMaxEditTextBytes - 1; // 255 ASCII bytes fit
    for (std::size_t i = 0; i < boundary.composition_length; ++i) {
        boundary.composition[i] = u'a';
    }
    boundary.composition[boundary.composition_length] = static_cast<char16_t>(0x4E2D); // 3 bytes
    boundary.composition_length += 1;
    if (!expect(composition_event(boundary, event), "boundary composition translates") ||
        !expect(event.text_size == kMaxEditTextBytes - 1,
                "truncation stops at the UTF-8 sequence boundary") ||
        !expect(event.text_truncated, "boundary composition reports truncation")) {
        return 9;
    }

    // The platform read truncation flag is carried through even when the payload is short.
    ImeStrings platform_truncated;
    platform_truncated.has_composition = true;
    platform_truncated.composition_truncated = true;
    platform_truncated.composition[0] = u'a';
    platform_truncated.composition_length = 1;
    if (!expect(composition_event(platform_truncated, event), "platform-truncated composition") ||
        !expect(event.text_truncated, "platform truncation flag carried")) {
        return 10;
    }

    // Both strings present: the caller emits committed text first and the composition state after.
    ImeStrings both;
    both.has_result = true;
    both.result[0] = u'R';
    both.result_length = 1;
    both.has_composition = true;
    both.composition[0] = u'C';
    both.composition_length = 1;
    InputEvent committed;
    InputEvent composing;
    if (!expect(result_event(both, committed) && composition_event(both, composing),
                "both strings translate") ||
        !expect(committed.type == InputEventType::EditText, "result is committed text") ||
        !expect(composing.type == InputEventType::CompositionUpdate, "composition is update") ||
        !expect(as_bytes(committed) == "R" && as_bytes(composing) == "C",
                "both payloads are independent")) {
        return 11;
    }

    // Sanity: converting no units yields an empty payload, not a crash.
    InputEvent none;
    const std::span<const char16_t> empty_units;
    tessera::hal::win32::write_utf8(empty_units, none);
    return expect(none.text_size == 0 && !none.text_truncated, "empty UTF-16 input") ? 0 : 12;
}
