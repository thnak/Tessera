// SPDX-License-Identifier: MIT

#include "utf16_map.hpp"

#include <algorithm>

namespace tessera::compiler::analysis {

namespace {

// Length of the well-formed UTF-8 sequence starting at `i`, or 0 when it is not well formed.
std::uint32_t sequence_length(std::string_view s, std::size_t i) {
    const auto lead = static_cast<unsigned char>(s[i]);
    std::uint32_t length = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
    } else {
        return 0;
    }
    if (i + length > s.size()) {
        return 0;
    }
    for (std::uint32_t k = 1; k < length; ++k) {
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0U) != 0x80U) {
            return 0;
        }
    }
    return length;
}

} // namespace

Utf16Map::Utf16Map(std::string_view utf8)
    : size_(static_cast<std::uint32_t>(utf8.size())) {
    if (utf8.size() >= 3 && utf8.substr(0, 3) == "\xEF\xBB\xBF") {
        bom_ = 3;
    }
    const bool ascii = std::ranges::all_of(
        utf8.substr(bom_), [](char c) { return static_cast<unsigned char>(c) < 0x80U; });
    if (ascii) {
        return;
    }
    units_.assign(utf8.size() + 1, 0);
    std::uint32_t units = 0;
    std::size_t i = bom_;
    while (i < utf8.size()) {
        const std::uint32_t length = std::max<std::uint32_t>(sequence_length(utf8, i), 1);
        for (std::uint32_t k = 0; k < length; ++k) {
            units_[i + k] = units;
        }
        units += length == 4 ? 2 : 1;
        i += length;
    }
    units_[utf8.size()] = units;
}

std::uint32_t Utf16Map::to_utf16(std::uint32_t byte) const {
    byte = std::min(byte, size_);
    if (byte <= bom_) {
        return 0;
    }
    return units_.empty() ? byte - bom_ : units_[byte];
}

std::uint32_t Utf16Map::to_byte(std::uint32_t unit) const {
    if (units_.empty()) {
        return std::min<std::uint32_t>(unit + bom_, size_);
    }
    // units_ is non-decreasing and a code point's bytes share its value, so the first byte holding
    // a value >= unit is a code point boundary.
    const auto it = std::lower_bound(units_.begin() + bom_, units_.end(), unit);
    return std::min(static_cast<std::uint32_t>(it - units_.begin()), size_);
}

} // namespace tessera::compiler::analysis
