// SPDX-License-Identifier: MIT
//
// Internal helpers shared by the navigation module's translation units (issue #196). Not a public
// header: nothing under `include/tessera/` includes it, and its contents are implementation detail
// the module may change without touching the recorded public surface.
//
// It exists because two TUs need the same two rules and duplicating them would let them drift:
//
//   * `split_path` is the one spelling of "how a path becomes segments", used by the pattern
//   compiler
//     (`navigation_route.cpp`) and by the matcher (`navigation_match.cpp`, which decodes what it
//     splits). Section 3's "decode/normalize once" only holds if both agree on where a segment
//     ends.
//   * the control-character and UTF-8 predicates are what `percent_decode` applies and what the
//   matcher
//     reports, so a single definition is what keeps a failure code pointing at the same class in
//     both.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw::detail {

// Splits `/a/b` into {"a","b"}, ignoring empty segments: a leading `/` and a trailing `/` both
// vanish, so `/orders` and `/orders/` are the same route. The caller that needs the raw (still
// percent-encoded) segments uses this; decoding is a separate pass, because a `%2f` must not create
// a segment.
[[nodiscard]] inline std::vector<std::string> split_path(std::string_view path) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    if (!path.empty() && path.front() == '/') {
        start = 1;
    }
    while (start <= path.size()) {
        const std::size_t end = path.find('/', start);
        const std::string_view segment = path.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!segment.empty()) {
            segments.emplace_back(segment);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return segments;
}

[[nodiscard]] inline bool is_control_byte(unsigned char value) noexcept {
    return value < 0x20U || value == 0x7FU;
}

// Strict UTF-8 well-formedness (Unicode table 3-7): overlong encodings, surrogates, values above
// U+10FFFF, bare continuation bytes and truncated sequences are all rejected. Layer 1 may not
// depend on `tessera_harness` (ADR-050 section 1), so the rule is restated here rather than linked;
// the suite feeds each class of malformed input through a deep link, which is what keeps the two
// spellings from drifting.
//
// On failure `error_offset` receives the offset of the first byte of the ill-formed sequence.
[[nodiscard]] inline bool is_valid_utf8(std::string_view text,
                                        std::size_t* error_offset = nullptr) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        char32_t minimum = 0;
        if (lead < 0x80U) {
            length = 1;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2;
            minimum = 0x80;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            length = 3;
            minimum = 0x800;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4;
            minimum = 0x10000;
        } else {
            if (error_offset != nullptr) {
                *error_offset = index;
            }
            return false;
        }
        if (length == 1) {
            index += 1;
            continue;
        }
        if (text.size() - index < length) {
            if (error_offset != nullptr) {
                *error_offset = index;
            }
            return false;
        }
        char32_t code_point = static_cast<char32_t>(lead & (0x7FU >> length));
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                if (error_offset != nullptr) {
                    *error_offset = index;
                }
                return false;
            }
            code_point = (code_point << 6U) | static_cast<char32_t>(next & 0x3FU);
        }
        const bool surrogate = code_point >= 0xD800U && code_point <= 0xDFFFU;
        if (code_point < minimum || code_point > 0x10FFFFU || surrogate) {
            if (error_offset != nullptr) {
                *error_offset = index;
            }
            return false;
        }
        index += length;
    }
    return true;
}

[[nodiscard]] inline bool is_hex_digit(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

[[nodiscard]] inline unsigned char hex_digit_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned char>(value - 'a' + 10);
    }
    return static_cast<unsigned char>(value - 'A' + 10);
}

} // namespace tessera::appfw::detail
