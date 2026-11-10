// SPDX-License-Identifier: MIT

// UTF-8 validation for decoded JSON strings: no overlong encodings, no surrogates, no code points
// above U+10FFFF. Kept separate from the parser so the byte-level rules stay reviewable.

#include "json.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::schema::internal {

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if ((lead & 0xE0) == 0xC0) {
            continuation_count = 1;
            code_point = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            continuation_count = 2;
            code_point = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            continuation_count = 3;
            code_point = lead & 0x07;
        } else {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            if (index + offset >= text.size()) {
                return false;
            }
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (continuation & 0x3F);
        }
        if ((continuation_count == 1 && code_point < 0x80) ||
            (continuation_count == 2 && code_point < 0x800) ||
            (continuation_count == 3 && code_point < 0x10000) || code_point > 0x10FFFF ||
            (code_point >= 0xD800 && code_point <= 0xDFFF)) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

} // namespace tessera::schema::internal
