// SPDX-License-Identifier: MIT
//
// Token decoding and typed param validation for the navigation module (docs/specs/navigation-v1.md
// sections 3 and 10; issue #196). Split from `navigation_route.cpp` so the table compiler and the
// codec stay each under the soft file limit and the untrusted-input rules read in one place.
//
// Everything here consumes untrusted bytes: a deep link's path, a route's segment, a param value.
// The rules are section 10's ("decode/validate, length limits, no traversal") plus section 3's
// "decode/ normalize once", and each failure has its own `NavigationErrorCode` so a caller and a
// test can name it.

#include "navigation_internal.hpp"
#include "tessera/appfw/navigation_route.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {
namespace {

using detail::hex_digit_value;
using detail::is_control_byte;
using detail::is_hex_digit;
using detail::is_valid_utf8;

} // namespace

std::expected<std::string, NavigationError> percent_decode(std::string_view text,
                                                           bool reject_separators) {
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character != '%') {
            decoded.push_back(character);
            continue;
        }
        if (index + 2 >= text.size() || !is_hex_digit(text[index + 1]) ||
            !is_hex_digit(text[index + 2])) {
            return std::unexpected(NavigationError{NavigationErrorCode::MalformedPercentEncoding,
                                                   std::string(text.substr(index, 3)), index});
        }
        const auto value = static_cast<unsigned char>((hex_digit_value(text[index + 1]) << 4U) |
                                                      hex_digit_value(text[index + 2]));
        // Section 10: "reject ... encoded separators/traversal". A decoded `/` or `\` inside a
        // segment would re-segment a path that was already matched; it is refused before the value
        // is used. (A caller that is decoding a *query value* passes `reject_separators = false`,
        // because a `/` in `?next=/orders` is data, not a segmentation risk.)
        if (reject_separators && (value == '/' || value == '\\')) {
            return std::unexpected(NavigationError{NavigationErrorCode::EncodedSeparator,
                                                   std::string(text.substr(index, 3)), index});
        }
        if (is_control_byte(value)) {
            return std::unexpected(NavigationError{NavigationErrorCode::ControlCharacter,
                                                   std::string(text.substr(index, 3)), index});
        }
        decoded.push_back(static_cast<char>(value));
        index += 2;
    }
    // A literal control byte survives the loop above (`%00` is caught, a raw 0x01 is not), so the
    // decoded form is swept too.
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        if (is_control_byte(static_cast<unsigned char>(decoded[index]))) {
            return std::unexpected(
                NavigationError{NavigationErrorCode::ControlCharacter, decoded, index});
        }
    }
    std::size_t bad_offset = 0;
    if (!is_valid_utf8(decoded, &bad_offset)) {
        return std::unexpected(
            NavigationError{NavigationErrorCode::InvalidUtf8, decoded, bad_offset});
    }
    return decoded;
}

bool validate_param_value(const ParamSpec& spec, std::string_view value) noexcept {
    switch (spec.type) {
    case ParamType::String:
        return true;
    case ParamType::Int: {
        if (value.empty()) {
            return false;
        }
        std::size_t index = 0;
        if (value.front() == '-' || value.front() == '+') {
            index = 1;
        }
        if (index >= value.size() || value.size() - index > 19) {
            return false; // no digits, or more than a signed 64-bit integer can hold
        }
        std::int64_t parsed = 0;
        for (; index < value.size(); ++index) {
            const char character = value[index];
            if (character < '0' || character > '9') {
                return false;
            }
            const int digit = character - '0';
            // Overflow check before the multiply: `parsed * 10 + digit` must not pass INT64_MAX.
            if (parsed > (INT64_MAX - digit) / 10) {
                return false;
            }
            parsed = parsed * 10 + digit;
        }
        return true;
    }
    case ParamType::Uuid: {
        // 8-4-4-4-12 hex, dashes exactly where RFC 4122 puts them; case is not significant.
        if (value.size() != 36) {
            return false;
        }
        for (std::size_t index = 0; index < value.size(); ++index) {
            const bool dash_here = index == 8 || index == 13 || index == 18 || index == 23;
            if (dash_here) {
                if (value[index] != '-') {
                    return false;
                }
                continue;
            }
            if (!is_hex_digit(value[index])) {
                return false;
            }
        }
        return true;
    }
    case ParamType::Enum: {
        for (const std::string& allowed : spec.allowed) {
            if (allowed == value) {
                return true;
            }
        }
        return false;
    }
    }
    return false;
}

const std::string* find_param(const ParamMap& params, std::string_view name) noexcept {
    for (const auto& [key, value] : params) {
        if (key == name) {
            return &value;
        }
    }
    return nullptr;
}

} // namespace tessera::appfw
