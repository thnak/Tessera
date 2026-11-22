// SPDX-License-Identifier: MIT

#pragma once

// Helpers shared by the two record-validation translation units (docs/specs/ir-v1.md section 2.2,
// ADR-051): the type-name shape rules, the record lookup, and the field-value form checks. Kept in
// one internal header so `validate_record.cpp` (type table) and `validate_record_value.cpp` (record
// values and dotted refs) agree byte-for-byte on what a type or field form is.

#include "validate_internal.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace tessera::compiler::ir::record_detail {

inline bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

inline bool is_primitive_type(std::string_view type) {
    for (const std::string_view primitive :
         {"bool", "int", "float", "string", "color", "length", "node_ref"}) {
        if (type == primitive) {
            return true;
        }
    }
    return false;
}

// A bare identifier: the spelling of a record name or of a payload record (`TextChangeEvent`).
inline bool is_identifier_type(std::string_view type) {
    if (type.empty() || (type.front() >= '0' && type.front() <= '9')) {
        return false;
    }
    for (const char c : type) {
        const bool word =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!word) {
            return false;
        }
    }
    return true;
}

// The inner type of `list<T>`/`optional<T>`, or nullopt.
inline std::optional<std::string_view> unwrap(std::string_view type, std::string_view wrapper) {
    if (!starts_with(type, wrapper) || type.size() <= wrapper.size() + 1 || type.back() != '>') {
        return std::nullopt;
    }
    return type.substr(wrapper.size(), type.size() - wrapper.size() - 1);
}

// The single type inside any list/optional wrappers: `list<optional<Size>>` -> `Size`.
inline std::string_view innermost_type(std::string_view type) {
    while (true) {
        if (const std::optional<std::string_view> list = unwrap(type, "list<")) {
            type = *list;
            continue;
        }
        if (const std::optional<std::string_view> optional = unwrap(type, "optional<")) {
            type = *optional;
            continue;
        }
        return type;
    }
}

// The declared record `name`, or null.
inline const RecordType* find_declared_type(const ValidationContext& context,
                                            std::string_view name) {
    const auto found = context.types.find(std::string(name));
    return found == context.types.end() ? nullptr : found->second;
}

// A record name: a declared record or a framework payload record (`callback` is a keyword type, not
// a record, so it has no record value).
inline bool is_record_name(const ValidationContext& context, std::string_view type) {
    return find_declared_type(context, type) != nullptr || is_predeclared_record(type);
}

// The animation-timing rules, shared with `validate_actions.cpp`: a generated tween must always
// finish (ir-v1.md section 2.1), so a duration is bounded, and an easing is a CSS keyword or a
// `cubic-bezier` whose x control points stay in [0, 1] (the rule that keeps the curve a function of
// time).
inline constexpr double kMaxAnimationMs = 60'000.0;
inline constexpr std::string_view kEasingKeywords[] = {"linear", "ease", "ease-in", "ease-out",
                                                       "ease-in-out"};

inline bool valid_cubic_bezier(std::string_view text) {
    constexpr std::string_view kPrefix = "cubic-bezier(";
    if (!text.starts_with(kPrefix) || !text.ends_with(')')) {
        return false;
    }
    const std::string body(text.substr(kPrefix.size(), text.size() - kPrefix.size() - 1));
    double values[4] = {};
    std::size_t start = 0;
    for (int index = 0; index < 4; ++index) {
        const std::size_t end = index == 3 ? body.size() : body.find(',', start);
        if (end == std::string::npos || end == start) {
            return false;
        }
        const std::string piece = body.substr(start, end - start);
        std::size_t used = 0;
        try {
            values[index] = std::stod(piece, &used);
        } catch (...) {
            return false;
        }
        while (used < piece.size() && piece[used] == ' ') {
            ++used;
        }
        if (used != piece.size() || !std::isfinite(values[index])) {
            return false;
        }
        start = end + 1;
    }
    return values[0] >= 0.0 && values[0] <= 1.0 && values[2] >= 0.0 && values[2] <= 1.0;
}

inline bool valid_easing(std::string_view text) {
    for (const std::string_view keyword : kEasingKeywords) {
        if (keyword == text) {
            return true;
        }
    }
    return valid_cubic_bezier(text);
}
} // namespace tessera::compiler::ir::record_detail
