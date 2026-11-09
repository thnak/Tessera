// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Pure IME translation decisions for Win32. This header deliberately does not include <windows.h>:
// the translator mirrors the composition strings and cursor into the plain structs below so the
// mapping is unit-testable without an HWND or an IME.

namespace tessera::hal::win32 {

// Maximum UTF-16 code units read from one composition/result string by the platform translator.
inline constexpr std::size_t kMaxImeStringUnits = 512;

struct ImeStrings {
    std::array<char16_t, kMaxImeStringUnits> composition{};
    std::size_t composition_length = 0;
    std::array<char16_t, kMaxImeStringUnits> result{};
    std::size_t result_length = 0;
    // ImmGetCompositionStringW(GCS_CURSORPOS): UTF-16 code units within the composition string.
    std::uint32_t cursor = 0;
    bool has_composition = false;
    bool has_result = false;
    // True when the platform string exceeded kMaxImeStringUnits and was cut on read.
    bool composition_truncated = false;
    bool result_truncated = false;
};

// Converts UTF-16 code units (surrogate pairs combined; lone surrogates become U+FFFD) into the
// bounded inline UTF-8 payload on `out`, setting `text_size` and `text_truncated`. The event type
// is left untouched. Output is truncated at a UTF-8 sequence boundary.
void write_utf8(std::span<const char16_t> units, ui::InputEvent& out);

// Fills `out` with an `EditText` event from GCS_RESULTSTR. Returns false for an absent or empty
// result string (nothing committed).
[[nodiscard]] bool result_event(const ImeStrings& strings, ui::InputEvent& out);

// Fills `out` with a `CompositionUpdate` event from GCS_COMPSTR, including an empty composition
// string (a valid update that clears the in-progress text). Returns false when no composition
// string was reported. The selection range is (cursor, cursor), clamped to the composition length,
// in UTF-16 code units.
[[nodiscard]] bool composition_event(const ImeStrings& strings, ui::InputEvent& out);

} // namespace tessera::hal::win32
