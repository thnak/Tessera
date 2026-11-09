// SPDX-License-Identifier: MIT

#include "win32_ime_translation.hpp"
#include "win32_input.hpp"

#include <algorithm>
#include <array>
#include <imm.h>

namespace tessera::hal {
void Win32InputTranslator::ime_composition(HWND window, LPARAM lparam) {
    const HIMC context = ImmGetContext(window);
    if (context == nullptr) {
        return;
    }

    win32::ImeStrings strings;
    // Reads a GCS_* string into the bounded buffer; returns true when the platform string was
    // longer than the buffer and was cut on read.
    const auto read = [context](DWORD kind, std::array<char16_t, win32::kMaxImeStringUnits>& buffer,
                                std::size_t& length) -> bool {
        const LONG bytes = ImmGetCompositionStringW(context, kind, nullptr, 0);
        if (bytes <= 0) {
            return false; // absent or empty
        }
        const std::size_t units = static_cast<std::size_t>(bytes) / sizeof(char16_t);
        const std::size_t copied_units = std::min(units, buffer.size());
        const LONG copied = ImmGetCompositionStringW(
            context, kind, buffer.data(), static_cast<DWORD>(copied_units * sizeof(char16_t)));
        if (copied <= 0) {
            return false;
        }
        length = static_cast<std::size_t>(copied) / sizeof(char16_t);
        return units > buffer.size();
    };

    const DWORD flags = static_cast<DWORD>(lparam);
    if ((flags & GCS_COMPSTR) != 0) {
        strings.has_composition = true;
        strings.composition_truncated =
            read(GCS_COMPSTR, strings.composition, strings.composition_length);
        const LONG cursor = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
        if (cursor > 0) {
            strings.cursor = static_cast<std::uint32_t>(cursor);
        }
    }
    if ((flags & GCS_RESULTSTR) != 0) {
        strings.has_result = true;
        strings.result_truncated = read(GCS_RESULTSTR, strings.result, strings.result_length);
    }
    ImmReleaseContext(window, context);

    ui::InputEvent event;
    // Committed text first, then the (usually empty) new composition state.
    if (win32::result_event(strings, event)) {
        push(event);
    }
    if (win32::composition_event(strings, event)) {
        push(event);
    }
}
} // namespace tessera::hal