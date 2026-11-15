// SPDX-License-Identifier: MIT

#pragma once

// Helpers shared by the two record-validation translation units (docs/specs/ir-v1.md section 2.2,
// ADR-051): the type-name shape rules, the record lookup, and the field-value form checks. Kept in
// one internal header so `validate_record.cpp` (type table) and `validate_record_value.cpp` (record
// values and dotted refs) agree byte-for-byte on what a type or field form is.

#include "validate_internal.hpp"

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

} // namespace tessera::compiler::ir::record_detail
