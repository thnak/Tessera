// SPDX-License-Identifier: MIT

// Slice 3 of ADR-051: an object literal in a declared record's position becomes an `ir::Record`
// (docs/specs/ir-v1.md section 4.3). The two positions V1 allows are a `useState` initial of a
// record type and a prop default of a record type; everywhere else the expression rules still
// reject an object literal.
//
// Field values are literals, nested records of literals and arrays of literals only (owner decision
// of 2026-11-13). A computed value is `TESSERA_IR_UNSUPPORTED` naming the member, not a silent
// drop: the IR schema already carries the field as an Expression, so a later slice can accept a
// dynamic value without a format change.
//
// A permitted field value never names a local, so this file lowers a field itself instead of going
// through the expression lowerer. That is what lets the hook path and the props path share it
// without either one's reference resolver.

#include "expression_lowering_internal.hpp"
#include "hook_lowering_internal.hpp"
#include "record_internal.hpp"
#include "tessera/compiler/jsx/parser.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::string_view kRecordValue = "TESSERA_IR_RECORD_VALUE";
constexpr std::string_view kUnsupported = "TESSERA_IR_UNSUPPORTED";

void fail_record(RecordLoweringState& state, std::string_view code, std::string message,
                 jsx::Span span, std::string_view suggestion = {}) {
    if (state.error) {
        return;
    }
    state.error = jsx::Diagnostic{code,
                                  jsx::Severity::Error,
                                  std::move(message),
                                  state.source_map.resolve(span),
                                  std::string(suggestion),
                                  std::nullopt,
                                  std::string(state.file)};
}

// The one field form this slice accepts, checked before lowering so the diagnostic names the member
// rather than repeating the generic expression error.
bool is_literal_field_value(const jsx::Expr& expr) {
    switch (expr.kind) {
    case jsx::ExprKind::Number:
    case jsx::ExprKind::String:
    case jsx::ExprKind::Boolean:
    case jsx::ExprKind::Null:
    case jsx::ExprKind::Object: // a nested record; its own members are checked where it is lowered
        return true;
    case jsx::ExprKind::Array:
        for (const jsx::ExprPtr& element : expr.operands) {
            if (!element || element->kind == jsx::ExprKind::Array ||
                !is_literal_field_value(*element)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

const ir::RecordMember* find_member(const ir::RecordType& record, std::string_view name) {
    for (const ir::RecordMember& member : record.members) {
        if (member.name == name) {
            return &member;
        }
    }
    return nullptr;
}

// A literal field value: `lower_literal` applies the int/float rule of jsx-subset-v1.md section 5
// and the int32 range check of ir-v1.md section 4.1, so a field value is typed exactly like a
// scalar default.
ir::ExprPtr lower_literal_field(RecordLoweringState& state, const jsx::Expr& expr,
                                const ir::RecordMember& member, std::string_view owner) {
    const ExpressionLoweringContext context{state.source_map, state.file, nullptr};
    LoweringState lowering{context, std::nullopt, {}, {}};
    ir::ExprPtr value = lower_literal(lowering, expr);
    if (value) {
        return value;
    }
    if (!state.error && lowering.error) {
        state.error = std::move(lowering.error);
    }
    if (!state.error) {
        fail_record(state, kRecordValue,
                    "field `" + member.name + "` of record `" + std::string(owner) +
                        "` has no IR value",
                    expr.span);
    }
    return nullptr;
}

ir::ExprPtr lower_field_value(RecordLoweringState& state, const jsx::Expr& expr,
                              const ir::RecordMember& member, std::string_view owner,
                              std::span<const ir::RecordType> records) {
    if (expr.kind == jsx::ExprKind::Object) {
        const std::optional<std::string_view> name = record_name_of(records, member.type);
        const ir::RecordType* nested = name == std::nullopt ? nullptr : find_record(records, *name);
        if (nested == nullptr) {
            fail_record(state, kUnsupported,
                        "field `" + member.name + "` of record `" + std::string(owner) +
                            "` is an object literal but its type `" + member.type +
                            "` is not a declared record",
                        expr.span);
            return nullptr;
        }
        return lower_record_value(state, expr, *nested, records);
    }
    if (expr.kind != jsx::ExprKind::Array) {
        return lower_literal_field(state, expr, member, owner);
    }
    // Section 4.3 names the array form "of the member element type", so an array where the member
    // is not a list is not one of the permitted forms at all.
    if (!list_element_of(member.type).has_value()) {
        fail_record(state, kUnsupported,
                    "field `" + member.name + "` of record `" + std::string(owner) + "` is `" +
                        member.type + "`, so an array is not one of its permitted forms",
                    expr.span);
        return nullptr;
    }
    std::vector<ir::ExprPtr> elements;
    elements.reserve(expr.operands.size());
    for (const jsx::ExprPtr& element : expr.operands) {
        if (!element) {
            fail_record(state, kRecordValue,
                        "an array field of record `" + std::string(owner) +
                            "` has a missing element",
                        expr.span);
            return nullptr;
        }
        if (element->kind == jsx::ExprKind::Object) {
            fail_record(state, kUnsupported,
                        "field `" + member.name + "` of record `" + std::string(owner) +
                            "` holds a list of records; only a list of literals is supported",
                        element->span);
            return nullptr;
        }
        ir::ExprPtr lowered = lower_literal_field(state, *element, member, owner);
        if (!lowered) {
            return nullptr;
        }
        elements.push_back(std::move(lowered));
    }
    return ir::make_array(std::move(elements));
}

} // namespace

// `{ ...state, field: value }` (ADR-066 item 3): a record update. The base must be a reference to a
// record of the same type, because the IR carries the base's id and the written members, not a copy
// of the whole record.
ir::ExprPtr lower_record_update(RecordLoweringState& state, const jsx::Expr& object,
                                const ir::RecordType& record,
                                std::span<const ir::RecordType> records) {
    if (!object.spread_base) {
        return nullptr;
    }
    const jsx::Expr& base = *object.spread_base;
    // The base is a reference chain (`state`, `props.form`), which is what the IR carries. Anything
    // else would need a temporary, which the record form does not have.
    if (base.kind != jsx::ExprKind::Identifier && base.kind != jsx::ExprKind::Member) {
        fail_record(state, kRecordValue,
                    "the base of a record update must be a reference to a record of type `" +
                        record.name + "`",
                    base.span);
        return nullptr;
    }
    std::vector<ir::RecordField> fields;
    for (const jsx::ObjectProperty& property : object.properties) {
        const ir::RecordMember* member = find_member(record, property.key);
        if (member == nullptr) {
            fail_record(state, kRecordValue,
                        "record `" + record.name + "` has no member `" + property.key + "`",
                        property.key_span);
            return nullptr;
        }
        if (!property.value) {
            fail_record(state, kRecordValue,
                        "field `" + property.key + "` of the record update has no value",
                        property.key_span);
            return nullptr;
        }
        ir::ExprPtr value =
            lower_field_value(state, *property.value, *member, record.name, records);
        if (!value) {
            return nullptr;
        }
        fields.push_back(ir::RecordField{property.key, std::move(value)});
    }
    // The base's IR id is the reference chain as written; the validator resolves it against the
    // component scope, so lowering does not duplicate that check.
    std::string id;
    const jsx::Expr* node = &base;
    while (node->kind == jsx::ExprKind::Member && node->operands.size() == 1 && node->operands[0]) {
        id = "." + node->text + id;
        node = node->operands[0].get();
    }
    if (node->kind != jsx::ExprKind::Identifier) {
        fail_record(state, kRecordValue,
                    "the base of a record update must be a reference to a record of type `" +
                        record.name + "`",
                    base.span);
        return nullptr;
    }
    // The written name is a source name (`state`), so it is resolved to its IR id (`state::state`)
    // through the same resolver every other reference uses.
    if (state.names == nullptr) {
        fail_record(state, kRecordValue,
                    "the base of a record update must be a reference to a record of type `" +
                        record.name + "`",
                    base.span);
        return nullptr;
    }
    const std::optional<std::string> resolved = state.names->resolve(node->text);
    if (!resolved) {
        fail_record(state, kRecordValue,
                    "the base of a record update must be a reference to a record of type `" +
                        record.name + "`",
                    base.span);
        return nullptr;
    }
    id = *resolved + id;
    return ir::make_record_update(record.name, std::move(id), std::move(fields));
}

ir::ExprPtr lower_record_value(RecordLoweringState& state, const jsx::Expr& object,
                               const ir::RecordType& record,
                               std::span<const ir::RecordType> records) {
    if (object.kind != jsx::ExprKind::Object) {
        fail_record(state, kRecordValue,
                    "an initial value of record type `" + record.name +
                        "` must be an object literal",
                    object.span);
        return nullptr;
    }
    // ADR-066 item 3: `{ ...state, field: value }` updates a record value. The base must be a
    // reference to a record of the same type, so the written members are the only thing that
    // changes and the result keeps the record's member order.
    if (object.spread_base) {
        return lower_record_update(state, object, record, records);
    }
    // An undeclared key is presumably a typo, not a new member, and a repeated key is ambiguous.
    // Both are checked before the members are walked, so the diagnostic names the offending key
    // rather than the first member that then looks missing.
    for (std::size_t i = 0; i < object.properties.size(); ++i) {
        const jsx::ObjectProperty& property = object.properties[i];
        if (find_member(record, property.key) == nullptr) {
            fail_record(state, kRecordValue,
                        "record `" + record.name + "` has no member `" + property.key + "`",
                        property.key_span);
            return nullptr;
        }
        for (std::size_t j = i + 1; j < object.properties.size(); ++j) {
            if (object.properties[j].key == property.key) {
                fail_record(state, kRecordValue,
                            "record `" + record.name + "` has the field `" + property.key +
                                "` twice",
                            object.properties[j].key_span);
                return nullptr;
            }
        }
    }
    // Fields follow the record's member order, not the source's: that is the generated aggregate's
    // designated-initializer order and the validator's member-order rule.
    std::vector<ir::RecordField> fields;
    for (const ir::RecordMember& member : record.members) {
        const jsx::ObjectProperty* property = nullptr;
        for (const jsx::ObjectProperty& candidate : object.properties) {
            if (candidate.key == member.name) {
                property = &candidate;
                break;
            }
        }
        if (property == nullptr) {
            if (member.optional) {
                continue; // an optional member is omitted and stays empty in the aggregate
            }
            fail_record(state, kRecordValue,
                        "record `" + record.name + "` is missing the required field `" +
                            member.name + "`",
                        object.span);
            return nullptr;
        }
        if (!property->value) {
            fail_record(state, kRecordValue,
                        "field `" + member.name + "` of record `" + record.name + "` has no value",
                        property->key_span);
            return nullptr;
        }
        // A shorthand (`{ top }`) parses as an Identifier value: a reference to a local, and a
        // dynamic field value is the unsupported form of this slice.
        if (!is_literal_field_value(*property->value)) {
            fail_record(state, kUnsupported,
                        "field `" + member.name + "` of record `" + record.name + "` is `" +
                            jsx::dump(*property->value) +
                            "`; this slice accepts a literal, a record of literals or an array of "
                            "literals",
                        property->value->span,
                        "write a literal value, for example `" + member.name + ": 4`");
            return nullptr;
        }
        ir::ExprPtr value =
            lower_field_value(state, *property->value, member, record.name, records);
        if (!value) {
            return nullptr;
        }
        fields.push_back(ir::RecordField{member.name, std::move(value)});
    }
    return ir::make_record(record.name, std::move(fields));
}

} // namespace tessera::compiler::lowering::internal
