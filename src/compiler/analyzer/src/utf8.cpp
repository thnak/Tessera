// SPDX-License-Identifier: MIT

#include "utf8.hpp"

namespace tessera::compiler::jsx {

std::size_t utf8_sequence_length(std::string_view text, std::size_t index) {
    if (index >= text.size()) {
        return 0;
    }
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80U) {
        return 1;
    }
    std::size_t length = 0;
    unsigned char second_min = 0x80;
    unsigned char second_max = 0xBF;
    if (lead >= 0xC2U && lead <= 0xDFU) {
        length = 2;
    } else if (lead >= 0xE0U && lead <= 0xEFU) {
        length = 3;
        if (lead == 0xE0U) {
            second_min = 0xA0; // reject overlong
        } else if (lead == 0xEDU) {
            second_max = 0x9F; // reject surrogates
        }
    } else if (lead >= 0xF0U && lead <= 0xF4U) {
        length = 4;
        if (lead == 0xF0U) {
            second_min = 0x90; // reject overlong
        } else if (lead == 0xF4U) {
            second_max = 0x8F; // reject > U+10FFFF
        }
    } else {
        return 0;
    }
    if (index + length > text.size()) {
        return 0;
    }
    const auto second = static_cast<unsigned char>(text[index + 1]);
    if (second < second_min || second > second_max) {
        return 0;
    }
    for (std::size_t i = 2; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(text[index + i]);
        if (byte < 0x80U || byte > 0xBFU) {
            return 0;
        }
    }
    return length;
}

void append_utf8(std::string& out, char32_t code_point) {
    const auto cp = static_cast<unsigned long>(code_point);
    if (cp < 0x80UL) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800UL) {
        out.push_back(static_cast<char>(0xC0UL | (cp >> 6)));
        out.push_back(static_cast<char>(0x80UL | (cp & 0x3FUL)));
    } else if (cp < 0x10000UL) {
        out.push_back(static_cast<char>(0xE0UL | (cp >> 12)));
        out.push_back(static_cast<char>(0x80UL | ((cp >> 6) & 0x3FUL)));
        out.push_back(static_cast<char>(0x80UL | (cp & 0x3FUL)));
    } else {
        out.push_back(static_cast<char>(0xF0UL | (cp >> 18)));
        out.push_back(static_cast<char>(0x80UL | ((cp >> 12) & 0x3FUL)));
        out.push_back(static_cast<char>(0x80UL | ((cp >> 6) & 0x3FUL)));
        out.push_back(static_cast<char>(0x80UL | (cp & 0x3FUL)));
    }
}

} // namespace tessera::compiler::jsx
