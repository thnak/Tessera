// SPDX-License-Identifier: MIT

#include "tessera/compiler/eval/number_text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>

namespace tessera::compiler::eval {
namespace {

constexpr std::int64_t kMaxDecimals = 20;
// A double's exact expansion has at most 1074 digits after the point; asking for more than that
// makes to_chars print every digit, which is what correct rounding needs.
constexpr int kExactFractionDigits = 1100;
constexpr std::size_t kExactBufferSize = 1500; // 309 integer digits + point + fraction

std::string special_text(double value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    return value < 0 ? "-Infinity" : "Infinity";
}

// Adds one unit in the last place to a string of decimal digits (no sign, no point).
void increment_digits(std::string& digits) {
    for (std::size_t i = digits.size(); i-- > 0;) {
        if (digits[i] != '9') {
            ++digits[i];
            return;
        }
        digits[i] = '0';
    }
    digits.insert(digits.begin(), '1');
}

} // namespace

std::string int_text(std::int32_t value) {
    std::array<char, 16> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}

std::string float_text(double value) {
    if (std::isnan(value) || std::isinf(value)) {
        return special_text(value);
    }
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), result.ptr);
}

std::string fixed_text(double value, std::int64_t decimals) {
    if (std::isnan(value) || std::isinf(value)) {
        return special_text(value);
    }
    const auto places =
        static_cast<std::size_t>(std::clamp<std::int64_t>(decimals, 0, kMaxDecimals));
    const bool negative = std::signbit(value);

    std::array<char, kExactBufferSize> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), std::fabs(value),
                      std::chars_format::fixed, kExactFractionDigits);
    const std::string exact(buffer.data(), result.ptr);
    const std::size_t point = exact.find('.');
    const std::string whole = exact.substr(0, point);
    const std::string fraction = exact.substr(point + 1);

    // Half away from zero on the magnitude: the first dropped digit decides, whatever follows it.
    const bool round_up = fraction[places] >= '5';
    std::string digits = whole + fraction.substr(0, places);
    if (round_up) {
        increment_digits(digits);
    }
    std::string text = digits.substr(0, digits.size() - places);
    if (places > 0) {
        text += '.';
        text += digits.substr(digits.size() - places);
    }
    return negative ? "-" + text : text;
}

} // namespace tessera::compiler::eval
