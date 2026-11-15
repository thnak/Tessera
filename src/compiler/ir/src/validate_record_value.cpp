// SPDX-License-Identifier: MIT

// Record value checks (docs/specs/ir-v1.md sections 4.3 and 9; ADR-051): the `record` expression,
// its field set and field values, the three positions a record value is allowed in, and a dotted
// `ref` into a declared record. The type table rules are in validate_record.cpp.

#include "validate_record_detail.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::ir {
namespace {

using record_detail::find_declared_type;
using record_detail::innermost_type;
using record_detail::is_identifier_type;
using record_detail::is_primitive_type;
using record_detail::is_record_name;
using record_detail::unwrap;

// True when the literal `value` satisfies the scalar `type` (section 4.1 conversions apply: an
// `int` literal converts to `float` exactly, never the reverse).
bool scalar_literal_matches(const Expr& value, std::string_view type) {
    if (value.kind != ExprKind::Literal) {
        return false;
    }
    if (type == "bool") {
        return std::holds_alternative<bool>(value.literal);
    }
    if (type == "int") {
        return std::holds_alternative<std::int64_t>(value.literal);
    }
    if (type == "float" || type == "length") {
        return std::holds_alternative<double>(value.literal) ||
               std::holds_alternative<std::int64_t>(value.literal);
    }
    if (type == "string" || type == "color") {
        return std::holds_alternative<std::string>(value.literal);
    }
    // `node_ref`, and a keyword type no record member may use, have no literal form.
    return false;
}

// True when `value` is a value of `type` in the first slice's forms (section 4.3): a literal, a
// nested record, or an array of literals of the element type.
bool value_matches(const ValidationContext& context, const Expr& value, std::string_view type) {
    if (const std::optional<std::string_view> element = unwrap(type, "list<")) {
        if (value.kind != ExprKind::Array) {
            return false;
        }
        for (const ExprPtr& item : value.operands) {
            // "an array of literals of the element type": a nested record element is not a literal,
            // so a `list<SomeRecord>` member is declarable but has no value form in this slice.
            if (!item || item->kind != ExprKind::Literal ||
                !scalar_literal_matches(*item, *element)) {
                return false;
            }
        }
        return true;
    }
    if (const std::optional<std::string_view> inner = unwrap(type, "optional<")) {
        // A null literal empties the member; otherwise the value is the inner type.
        if (value.kind == ExprKind::Literal && std::holds_alternative<Null>(value.literal)) {
            return true;
        }
        return value_matches(context, value, *inner);
    }
    if (is_record_name(context, type)) {
        return value.kind == ExprKind::Record;
    }
    return scalar_literal_matches(value, type);
}

// Section 4.3: a field value is a literal, a nested record of literals, or an array of literals.
bool supported_field_value(const Expr& value) {
    switch (value.kind) {
    case ExprKind::Literal:
    case ExprKind::Record:
        return true;
    case ExprKind::Array:
        for (const ExprPtr& item : value.operands) {
            if (!item || item->kind != ExprKind::Literal) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

const RecordMember* find_member(const RecordType& type, std::string_view name, std::size_t& index) {
    for (std::size_t i = 0; i < type.members.size(); ++i) {
        if (type.members[i].name == name) {
            index = i;
            return &type.members[i];
        }
    }
    return nullptr;
}

} // namespace

void validate_record_expr(ValidationContext& context, const Expr& expr, const std::string* expected,
                          const SourceSpan& where) {
    if (expr.type.empty()) {
        context.report("TESSERA_IR_MALFORMED", "a record value needs a record type", where);
        return;
    }
    const RecordType* type = find_declared_type(context, expr.type);
    if (type == nullptr) {
        // The type table is the only source of record values: a payload record is framework data
        // with no aggregate form, and an undeclared name is section 2.2's unknown-type rule.
        if (is_predeclared_record(expr.type)) {
            context.report("TESSERA_IR_UNSUPPORTED",
                           "the framework payload record `" + expr.type +
                               "` is not a project record and has no record value",
                           where);
        } else {
            context.report("TESSERA_IR_UNKNOWN_TYPE",
                           "record value type `" + expr.type + "` is not in the type table", where);
        }
        return;
    }
    context.used_types.insert(expr.type);
    if (expected != nullptr) {
        const std::string_view wanted = innermost_type(*expected);
        if (context.types.count(std::string(wanted)) == 0 || wanted != expr.type) {
            context.report("TESSERA_IR_RECORD_VALUE",
                           "a `" + expr.type + "` record value does not match the declared type `" +
                               std::string(wanted) + "`",
                           where);
        }
    }
    // Field set: every non-optional member exactly once, in member order; an optional member may be
    // omitted (its value is empty in the generated aggregate).
    std::unordered_set<std::string> seen;
    std::size_t last_index = 0;
    bool first = true;
    for (const RecordField& field : expr.fields) {
        std::size_t index = 0;
        const RecordMember* member = find_member(*type, field.name, index);
        if (member == nullptr) {
            context.report("TESSERA_IR_RECORD_VALUE",
                           "record `" + expr.type + "` has no member `" + field.name + "`", where);
            continue;
        }
        if (!seen.insert(field.name).second) {
            context.report("TESSERA_IR_RECORD_VALUE",
                           "duplicate field `" + field.name + "` of record `" + expr.type + "`",
                           where);
            continue;
        }
        if (!first && index <= last_index) {
            context.report("TESSERA_IR_ORDER",
                           "the fields of record `" + expr.type + "` follow member order", where);
        }
        first = false;
        last_index = index;
        if (!field.value) {
            context.report("TESSERA_IR_MALFORMED",
                           "field `" + field.name + "` of record `" + expr.type + "` has no value",
                           where);
            continue;
        }
        if (!supported_field_value(*field.value)) {
            context.report("TESSERA_IR_UNSUPPORTED",
                           "field `" + field.name + "` of record `" + expr.type +
                               "` is a literal, a record or an array of literals in this slice",
                           where);
            continue;
        }
        // Section 4.3 names three field forms: a literal, a nested record, and an array of literals
        // *of the element type*. An array where the member is not a list is therefore not one of
        // the permitted forms at all, which is the unsupported-form rule, not a value/type
        // mismatch.
        if (field.value->kind == ExprKind::Array && !unwrap(member->type, "list<").has_value()) {
            context.report("TESSERA_IR_UNSUPPORTED",
                           "field `" + field.name + "` of record `" + expr.type + "` is `" +
                               member->type + "`, so an array is not one of its permitted forms",
                           where);
            continue;
        }
        if (field.value->kind == ExprKind::Record) {
            validate_record_expr(context, *field.value, &member->type, where);
            continue;
        }
        if (!value_matches(context, *field.value, member->type)) {
            context.report("TESSERA_IR_RECORD_VALUE",
                           "field `" + field.name + "` of record `" + expr.type + "` must be `" +
                               member->type + "`",
                           where);
            continue;
        }
        // A literal field value is still checked for e.g. a non-finite float (section 9).
        validate_expr(context, ComponentScope{}, {}, field.value.get(), where, true);
    }
    for (const RecordMember& member : type->members) {
        if (!member.optional && seen.count(member.name) == 0) {
            context.report("TESSERA_IR_RECORD_VALUE",
                           "record `" + expr.type + "` is missing the required member `" +
                               member.name + "`",
                           where);
        }
    }
}

void validate_record_position(ValidationContext& context, const ComponentScope& scope,
                              const Expr* expr, const std::string& declared_type,
                              const SourceSpan& where) {
    if (expr == nullptr) {
        return;
    }
    // Only a *project* record has a value form: a framework payload record is a valid type name but
    // its values come from the runtime, so a payload-typed position takes any ordinary expression
    // and never a `record` value.
    if (find_declared_type(context, innermost_type(declared_type)) != nullptr) {
        if (expr->kind != ExprKind::Record) {
            context.report("TESSERA_IR_RECORD_VALUE",
                           "an initial value of record type `" + declared_type +
                               "` must be a record value",
                           where);
            return;
        }
        validate_record_expr(context, *expr, &declared_type, where);
        return;
    }
    // Not a record position: a record value here is not one of the three places section 4.3 allows.
    validate_expr(context, scope, {}, expr, where);
}

void validate_reference_member(ValidationContext& context, const ComponentScope& scope,
                               std::string_view id, const SourceSpan& where) {
    const std::size_t dot = id.find('.');
    if (dot == std::string_view::npos) {
        return;
    }
    const auto declared = scope.declared_types.find(std::string(id.substr(0, dot)));
    if (declared == scope.declared_types.end()) {
        // A loop item or a handler parameter: its element type is not a module-level record.
        return;
    }
    // Walk each dotted segment through the declared records; a payload record's members are element
    // schema data, so the walk stops there (section 9.1, "deliberately not covered").
    std::string current = declared->second;
    std::string_view rest = id.substr(dot + 1);
    while (true) {
        const std::string_view inner = innermost_type(current);
        if (inner != current) {
            if (unwrap(current, "list<").has_value()) {
                context.report("TESSERA_IR_UNRESOLVED",
                               "`" + std::string(id) + "` indexes a list member", where);
                return;
            }
            current = std::string(inner);
        }
        const RecordType* type = find_declared_type(context, current);
        if (type == nullptr) {
            return; // A payload record or a non-record base: not member-checked in this slice.
        }
        const std::size_t next = rest.find('.');
        const std::string_view segment = rest.substr(0, next);
        std::size_t index = 0;
        const RecordMember* member = find_member(*type, segment, index);
        if (member == nullptr) {
            context.report("TESSERA_IR_UNRESOLVED",
                           "`" + std::string(id) + "` reads `" + std::string(segment) +
                               "`, which record `" + current + "` does not have",
                           where);
            return;
        }
        context.used_types.insert(current);
        if (next == std::string_view::npos) {
            return;
        }
        current = member->type;
        rest = rest.substr(next + 1);
    }
}

} // namespace tessera::compiler::ir
