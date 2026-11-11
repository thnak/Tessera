// SPDX-License-Identifier: MIT

#include "win32_ime_translation.hpp"

#include <algorithm>

namespace tessera::hal::win32 {
namespace {

constexpr char32_t kReplacementCharacter = 0xFFFD;
constexpr char32_t kSurrogateBase = 0x10000;
constexpr std::uint32_t kHighSurrogateFirst = 0xD800;
constexpr std::uint32_t kHighSurrogateLast = 0xDBFF;
constexpr std::uint32_t kLowSurrogateFirst = 0xDC00;
constexpr std::uint32_t kLowSurrogateLast = 0xDFFF;

bool is_high_surrogate(char16_t unit) {
    return unit >= kHighSurrogateFirst && unit <= kHighSurrogateLast;
}

bool is_low_surrogate(char16_t unit) {
    return unit >= kLowSurrogateFirst && unit <= kLowSurrogateLast;
}

void append_codepoint(char32_t codepoint, ui::InputEvent& out) {
    char bytes[4];
    std::size_t size = 0;
    if (codepoint <= 0x7F) {
        bytes[size++] = static_cast<char>(codepoint);
    } else if (codepoint <= 0x7FF) {
        bytes[size++] = static_cast<char>(0xC0 | (codepoint >> 6));
        bytes[size++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        bytes[size++] = static_cast<char>(0xE0 | (codepoint >> 12));
        bytes[size++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[size++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        bytes[size++] = static_cast<char>(0xF0 | (codepoint >> 18));
        bytes[size++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        bytes[size++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[size++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    if (static_cast<std::size_t>(out.text_size) + size > ui::kMaxEditTextBytes) {
        out.text_truncated = true;
        return;
    }
    for (std::size_t i = 0; i < size; ++i) {
        out.text[out.text_size + i] = bytes[i];
    }
    out.text_size += static_cast<std::uint32_t>(size);
}

} // namespace

void write_utf8(std::span<const char16_t> units, ui::InputEvent& out) {
    out.text = {};
    out.text_size = 0;
    out.text_truncated = false;
    std::size_t index = 0;
    while (index < units.size() && !out.text_truncated) {
        std::uint32_t unit = static_cast<std::uint32_t>(units[index]);
        char32_t codepoint = 0;
        if (unit >= kHighSurrogateFirst && unit <= kHighSurrogateLast && index + 1 < units.size()) {
            const std::uint32_t low = static_cast<std::uint32_t>(units[index + 1]);
            if (low >= kLowSurrogateFirst && low <= kLowSurrogateLast) {
                codepoint =
                    static_cast<char32_t>(kSurrogateBase + ((unit - kHighSurrogateFirst) << 10) +
                                          (low - kLowSurrogateFirst));
                index += 2;
            } else {
                codepoint = kReplacementCharacter;
                ++index;
            }
        } else if ((unit >= kHighSurrogateFirst && unit <= kHighSurrogateLast) ||
                   (unit >= kLowSurrogateFirst && unit <= kLowSurrogateLast)) {
            // Lone surrogate half: substitute rather than emit invalid UTF-8.
            codepoint = kReplacementCharacter;
            ++index;
        } else {
            codepoint = static_cast<char32_t>(unit);
            ++index;
        }
        append_codepoint(codepoint, out);
    }
}

bool result_event(const ImeStrings& strings, ui::InputEvent& out) {
    if (!strings.has_result || strings.result_length == 0) {
        return false;
    }
    out = ui::InputEvent{};
    out.type = ui::InputEventType::EditText;
    write_utf8(std::span<const char16_t>(strings.result.data(), strings.result_length), out);
    // The inline cap is a byte cap, so a non-empty string always yields at least one byte.
    out.text_truncated = out.text_truncated || strings.result_truncated;
    return true;
}

bool composition_event(const ImeStrings& strings, ui::InputEvent& out) {
    if (!strings.has_composition) {
        return false;
    }
    out = ui::InputEvent{};
    out.type = ui::InputEventType::CompositionUpdate;
    write_utf8(std::span<const char16_t>(strings.composition.data(), strings.composition_length),
               out);
    out.text_truncated = out.text_truncated || strings.composition_truncated;
    const std::uint32_t units = static_cast<std::uint32_t>(
        std::min<std::size_t>(strings.composition_length, kMaxImeStringUnits));
    std::uint32_t cursor_units = std::min(strings.cursor, units);
    if (cursor_units > 0 && cursor_units < units &&
        is_low_surrogate(strings.composition[cursor_units]) &&
        is_high_surrogate(strings.composition[cursor_units - 1])) {
        --cursor_units; // inside a surrogate pair: snap to the start of the code point
    }
    // The payload is UTF-8, so the caret is the byte length of the converted prefix.
    ui::InputEvent prefix;
    write_utf8(std::span<const char16_t>(strings.composition.data(), cursor_units), prefix);
    const std::uint32_t cursor = std::min(prefix.text_size, out.text_size);
    out.selection_start = cursor;
    out.selection_end = cursor;
    return true;
}

} // namespace tessera::hal::win32
