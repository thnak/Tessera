// SPDX-License-Identifier: MIT

#include "tessera/harness/utf8.hpp"

#include <cstdint>

namespace tessera::harness {
namespace {

// 1 marks an ASCII byte, 0 an invalid lead byte. Two-byte leads below 0xC2 would encode an
// overlong value; leads above 0xF4 would encode past U+10FFFF, so both are rejected up front.
struct LeadInfo {
    std::uint8_t length = 0;
    char32_t minimum = 0;
};

constexpr LeadInfo classify_lead(std::uint8_t lead) noexcept {
    if (lead < 0x80U) {
        return {1, 0};
    }
    if (lead >= 0xC2U && lead <= 0xDFU) {
        return {2, 0x80};
    }
    if (lead >= 0xE0U && lead <= 0xEFU) {
        return {3, 0x800};
    }
    if (lead >= 0xF0U && lead <= 0xF4U) {
        return {4, 0x10000};
    }
    return {};
}

bool is_continuation(std::uint8_t byte) noexcept {
    return (byte & 0xC0U) == 0x80U;
}

} // namespace

bool is_valid_utf8(std::string_view text, std::size_t* error_offset) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<std::uint8_t>(text[index]);
        const LeadInfo info = classify_lead(lead);
        if (info.length == 0) {
            if (error_offset != nullptr) {
                *error_offset = index;
            }
            return false;
        }
        if (info.length > 1) {
            if (text.size() - index < info.length) {
                if (error_offset != nullptr) {
                    *error_offset = index;
                }
                return false;
            }
            // The lead byte's low bits are the high bits of the scalar value.
            char32_t code_point = lead & (0x7FU >> info.length);
            for (std::uint8_t offset = 1; offset < info.length; ++offset) {
                const auto next = static_cast<std::uint8_t>(text[index + offset]);
                if (!is_continuation(next)) {
                    if (error_offset != nullptr) {
                        *error_offset = index;
                    }
                    return false;
                }
                code_point = (code_point << 6U) | (next & 0x3FU);
            }
            const bool surrogate = code_point >= 0xD800U && code_point <= 0xDFFFU;
            if (code_point < info.minimum || code_point > 0x10FFFFU || surrogate) {
                if (error_offset != nullptr) {
                    *error_offset = index;
                }
                return false;
            }
        }
        index += info.length;
    }
    return true;
}

bool append_utf8(std::string& out, char32_t code_point) {
    if (code_point > 0x10FFFFU || (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
        return false;
    }
    if (code_point < 0x80U) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
    return true;
}

} // namespace tessera::harness
