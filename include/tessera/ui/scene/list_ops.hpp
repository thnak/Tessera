// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
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

} // namespace tessera::ui
