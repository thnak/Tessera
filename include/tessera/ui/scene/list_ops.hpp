// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

// The list operations of the pure-expression allowlist (ADR-066 item 3,
// docs/specs/ir-v1-expr.md section 4.2). Generated code calls these instead of writing the loops
// inline, so the generated C++ and the IR evaluator share one definition of each rule: a bound is
// clamped rather than an error, and a predicate sees one element at a time.
namespace tessera::ui {

// `list.filter(item => <bool>)`: the elements the predicate keeps, in order. The predicate is a
// callable taking one element, so the generated lambda and a hand-written one are the same thing.
template<typename T, typename Predicate>
[[nodiscard]] std::vector<T> filter_list(const std::vector<T>& list, Predicate predicate) {
    std::vector<T> kept;
    kept.reserve(list.size());
    for (const T& item : list) {
        if (predicate(item)) {
            kept.push_back(item);
        }
    }
    return kept;
}

// `list.slice(start[, end])`: the elements in `[start, end)`, with both bounds clamped to the list
// as in JavaScript, so a negative or oversized bound is not an error. `end` defaults to the size.
[[nodiscard]] inline std::vector<std::int32_t> slice_bounds(std::int64_t start, std::int64_t end,
                                                            std::int64_t size) {
    const std::int64_t from = std::clamp<std::int64_t>(start, 0, size);
    const std::int64_t to = std::clamp<std::int64_t>(end, from, size);
    return {static_cast<std::int32_t>(from), static_cast<std::int32_t>(to)};
}

template<typename T>
[[nodiscard]] std::vector<T> slice_list(const std::vector<T>& list, std::int64_t start) {
    const std::vector<std::int32_t> bounds = slice_bounds(
        start, static_cast<std::int64_t>(list.size()), static_cast<std::int64_t>(list.size()));
    return std::vector<T>(list.begin() + bounds[0], list.begin() + bounds[1]);
}

template<typename T>
[[nodiscard]] std::vector<T> slice_list(const std::vector<T>& list, std::int64_t start,
                                        std::int64_t end) {
    const std::vector<std::int32_t> bounds =
        slice_bounds(start, end, static_cast<std::int64_t>(list.size()));
    return std::vector<T>(list.begin() + bounds[0], list.begin() + bounds[1]);
}

// The total string methods of ADR-066 item 3. Each is ASCII-only and locale-independent, so the
// generated code and the IR evaluator produce the same bytes for the same input.
[[nodiscard]] inline std::string string_trim(const std::string& text) {
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && space(text[begin])) {
        ++begin;
    }
    while (end > begin && space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

[[nodiscard]] inline std::string string_toUpperCase(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 'a' && byte <= 'z') {
            c = static_cast<char>(byte - 'a' + 'A');
        }
    }
    return out;
}

[[nodiscard]] inline std::string string_toLowerCase(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 'A' && byte <= 'Z') {
            c = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return out;
}

[[nodiscard]] inline bool string_startsWith(const std::string& text, const std::string& prefix) {
    return text.starts_with(prefix);
}

[[nodiscard]] inline std::string string_slice(const std::string& text, std::int64_t start) {
    const std::vector<std::int32_t> bounds = slice_bounds(
        start, static_cast<std::int64_t>(text.size()), static_cast<std::int64_t>(text.size()));
    return text.substr(static_cast<std::size_t>(bounds[0]),
                       static_cast<std::size_t>(bounds[1] - bounds[0]));
}

[[nodiscard]] inline std::string string_slice(const std::string& text, std::int64_t start,
                                              std::int64_t end) {
    const std::vector<std::int32_t> bounds =
        slice_bounds(start, end, static_cast<std::int64_t>(text.size()));
    return text.substr(static_cast<std::size_t>(bounds[0]),
                       static_cast<std::size_t>(bounds[1] - bounds[0]));
}

// `n.toFixed(d)`: `d` decimals, ties away from zero, no thousands separator. `d` is clamped to
// 0..20, and a non-finite value prints as `NaN`/`Infinity`/`-Infinity`, which is what the IR
// evaluator's `formatNumber` does.
[[nodiscard]] inline std::string string_toFixed(double value, std::int32_t decimals) {
    const std::int32_t digits = std::clamp<std::int32_t>(decimals, 0, 20);
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value < 0 ? "-Infinity" : "Infinity";
    }
    char buffer[512];
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, digits);
    if (result.ec != std::errc{}) {
        return {};
    }
    return std::string(buffer, result.ptr);
}

// Issue #803: the strict whole-string number parse behind `parseIntOr`/`parseFloatOr`. The text is
// trimmed, then must be *entirely* a number: an optional sign and decimal digits, with a single
// decimal point for the float form, or the fallback wins. No leading scan (JavaScript
// `parseInt` would read `12` out of `12abc`), no exponent, no hex: those are unparseable
// text, not numbers in another notation. Out of range saturates rather than falling back,
// because the string *is* a number and the fallback is for the not-a-number case. Inline and
// shared with the IR evaluator, like every helper here, so the generated C++ and the evaluator
// agree byte for byte.
//
// The character scan happens before `from_chars` because `from_chars` for a float accepts an
// exponent (`1e3`) and for an integer rejects a leading `+` (`+13`), neither of which is this
// rule. The scan decides *what is a number*; `from_chars` only converts the already-validated
// digits.
template<typename T> [[nodiscard]] inline T parse_number_or(std::string_view text, T fallback) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (begin < end && space(text[begin])) {
        ++begin;
    }
    while (end > begin && space(text[end - 1])) {
        --end;
    }
    const std::string_view body = text.substr(begin, end - begin);
    if (body.empty()) {
        return fallback;
    }
    std::size_t at = 0;
    bool negative = false;
    if (body[at] == '+' || body[at] == '-') {
        // One optional sign, consumed here: `from_chars` accepts `-` for integers but not `+`, and
        // `chars_format::fixed` accepts neither, so both forms parse from the digits alone and the
        // sign is applied to the value.
        negative = body[at] == '-';
        ++at;
    }
    std::size_t digits = 0;
    bool point = false;
    for (; at < body.size(); ++at) {
        const char c = body[at];
        if (c >= '0' && c <= '9') {
            ++digits;
            continue;
        }
        if (c == '.' && !point) {
            point = true;
            continue;
        }
        return fallback; // anything else: unparseable text, not a number in another notation
    }
    if (digits == 0) {
        return fallback; // a sign alone, or a bare point, is not a number
    }
    // `from_chars` over the validated digits. The float overload takes the format and the integer
    // one does not, so `if constexpr` dispatches; `fixed`, not `general`, because `general` would
    // accept the exponent this rule rejects. The integral path parses wide and clamps, because
    // out of range **saturates** (the string *is* a number) and a direct `T` parse reports
    // out-of-range as an error instead.
    const char* const digits_first = body.data() + (negative || body[0] == '+' ? 1 : 0);
    const char* const digits_last = body.data() + body.size();
    if constexpr (std::is_integral_v<T>) {
        std::int64_t wide = 0;
        const auto [ptr, ec] = std::from_chars(digits_first, digits_last, wide, 10);
        if (ec == std::errc::result_out_of_range) {
            // The digits are a number, so the value saturates in the sign's direction.
            return negative ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
        }
        if (ec != std::errc{} || ptr != digits_last) {
            return fallback;
        }
        const std::int64_t signed_value = negative ? -wide : wide;
        return static_cast<T>(std::clamp(signed_value,
                                         static_cast<std::int64_t>(std::numeric_limits<T>::min()),
                                         static_cast<std::int64_t>(std::numeric_limits<T>::max())));
    } else {
        // `fixed`, not `general`: `general` would accept the exponent this rule rejects.
        T parsed{};
        const auto [ptr, ec] =
            std::from_chars(digits_first, digits_last, parsed, std::chars_format::fixed);
        if (ec != std::errc{} || ptr != digits_last) {
            return fallback;
        }
        return negative ? static_cast<T>(-parsed) : parsed;
    }
}

} // namespace tessera::ui
