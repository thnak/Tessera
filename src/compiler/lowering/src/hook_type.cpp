// SPDX-License-Identifier: MIT

// The type fields slice 6 has to fill: `state[].type` and `animatedValues[].type` of
// docs/specs/ir-v1.md section 2, and the written parameter and return types of a native handler
// (section 6). Types are copied from the source syntax; the type table, alias resolution and
// expression typing are slice 4 and are not implemented here.

#include "expression_lowering_internal.hpp"
#include "hook_lowering_internal.hpp"

#include <string>
#include <utility>

namespace tessera::compiler::lowering::internal {
namespace {

bool is_null_name(const jsx::TypeNode& type) {
    return type.kind == jsx::TypeKind::Name && type.children.empty() && type.text == "null";
}

} // namespace

std::optional<std::string> lower_written_type(const jsx::TypeNode& type) {
    switch (type.kind) {
    case jsx::TypeKind::Name:
        if (!type.children.empty() || type.text.empty()) {
            // Generic types have no IR spelling in V1 except the array wrapper below.
            return std::nullopt;
        }
        if (type.text == "int" || type.text == "float" || type.text == "string") {
            return type.text;
        }
        if (type.text == "boolean" || type.text == "bool") {
            return std::string("bool");
        }
        if (type.text == "number" || type.text == "null") {
            // `number` is `TESSERA_JSX_TYPE_NUMBER` in the type pass; a bare `null` has no IR type.
            return std::nullopt;
        }
        // A record or alias name. The IR validator accepts any identifier as a record type name
        // until the type table exists, so copying it is the least committal choice.
        return type.text;
    case jsx::TypeKind::StringLiteral:
        return std::string("string");
    case jsx::TypeKind::Array: {
        if (type.children.size() != 1 || !type.children[0]) {
            return std::nullopt;
        }
        const std::optional<std::string> element = lower_written_type(*type.children[0]);
        return element ? std::optional<std::string>("list<" + *element + ">") : std::nullopt;
    }
    case jsx::TypeKind::Union: {
        // `T | null` lowers to optional<T>; a union of string literals is the enum type, which the
        // IR carries as string until enums get a form of their own.
        bool all_string_literals = !type.children.empty();
        for (const jsx::TypePtr& child : type.children) {
            all_string_literals =
                all_string_literals && child && child->kind == jsx::TypeKind::StringLiteral;
        }
        if (all_string_literals) {
            return std::string("string");
        }
        if (type.children.size() == 2 && type.children[0] && type.children[1]) {
            const jsx::TypeNode& first = *type.children[0];
            const jsx::TypeNode& second = *type.children[1];
            const jsx::TypeNode* inner = nullptr;
            if (is_null_name(second)) {
                inner = &first;
            } else if (is_null_name(first)) {
                inner = &second;
            }
            if (inner != nullptr) {
                const std::optional<std::string> value = lower_written_type(*inner);
                return value ? std::optional<std::string>("optional<" + *value + ">")
                             : std::nullopt;
            }
        }
        return std::nullopt;
    }
    case jsx::TypeKind::Object:
    case jsx::TypeKind::Function:
        // An inline structural type needs a declared record name from the type table (slice 4).
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> lower_return_type(const jsx::TypePtr& type) {
    if (!type) {
        return std::string();
    }
    if (type->kind == jsx::TypeKind::Name && type->children.empty() && type->text == "void") {
        return std::string();
    }
    return lower_written_type(*type);
}

std::optional<std::string> initial_literal_type(const jsx::Expr& expr) {
    switch (expr.kind) {
    case jsx::ExprKind::Number:
        return std::string(has_fraction_or_exponent(expr.text) ? "float" : "int");
    case jsx::ExprKind::String:
        return std::string("string");
    case jsx::ExprKind::Boolean:
        return std::string("bool");
    case jsx::ExprKind::Null:
    default:
        // `null` alone has no IR type, and any other expression needs expression typing (slice 4).
        return std::nullopt;
    }
}

} // namespace tessera::compiler::lowering::internal
