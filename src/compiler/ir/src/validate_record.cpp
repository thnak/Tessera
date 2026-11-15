// SPDX-License-Identifier: MIT

// Record type table and record value checks (docs/specs/ir-v1.md sections 2.2, 4.3, 9 and 11;
// ADR-051). Split out of validate_expr.cpp because the record rules are a family of their own and
// that file is already close to the line limit.

#include "validate_internal.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace tessera::compiler::ir {
namespace {

constexpr std::string_view kTypePrefix = "type::";

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

bool is_primitive_type(std::string_view type) {
    for (const std::string_view primitive :
         {"bool", "int", "float", "string", "color", "length", "node_ref"}) {
        if (type == primitive) {
            return true;
        }
    }
    return false;
}

// A bare identifier: the spelling of a record name or of a payload record (`TextChangeEvent`).
bool is_identifier_type(std::string_view type) {
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
std::optional<std::string_view> unwrap(std::string_view type, std::string_view wrapper) {
    if (!starts_with(type, wrapper) || type.size() <= wrapper.size() + 1 || type.back() != '>') {
        return std::nullopt;
    }
    return type.substr(wrapper.size(), type.size() - wrapper.size() - 1);
}

// The declared record `name`, or null.
const RecordType* find_declared_type(const ValidationContext& context, std::string_view name) {
    const auto found = context.types.find(std::string(name));
    return found == context.types.end() ? nullptr : found->second;
}

// A record name: a declared record or a framework payload record (`callback` is a keyword type, not
// a record, so it has no record value).
bool is_record_name(const ValidationContext& context, std::string_view type) {
    return find_declared_type(context, type) != nullptr || is_predeclared_record(type);
}

// The single type inside any list/optional wrappers: `list<optional<Size>>` -> `Size`.
std::string_view innermost_type(std::string_view type) {
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

bool literal_matches(const ValidationContext& context, const Expr& value, std::string_view type) {
    if (const std::optional<std::string_view> element = unwrap(type, "list<")) {
        // A list member takes an `array` of literals of the element type (section 4.3: "an array of
        // literals of the element type"). A nested record element is not a literal, so a
        // `list<SomeRecord>` member is declarable but has no value form in this slice.
        if (value.kind != ExprKind::Array) {
            return false;
        }
        for (const ExprPtr& item : value.operands) {
            if (!item || item->kind != ExprKind::Literal ||
                !literal_matches(context, *item, *element)) {
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
        return literal_matches(context, value, *inner);
    }
    if (is_record_name(context, type)) {
        // A member of record type takes a nested record value, checked by its own call.
        return value.kind == ExprKind::Record;
    }
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
        // Section 4.1: an `int` converts to `float` exactly; the reverse is never implicit.
        return std::holds_alternative<double>(value.literal) ||
               std::holds_alternative<std::int64_t>(value.literal);
    }
    if (type == "string" || type == "color") {
        return std::holds_alternative<std::string>(value.literal);
    }
    // `node_ref`, and a keyword type no record member may use, have no literal form.
    return false;
}

// Section 4.3: a field value is a literal, a nested record of literals, or an array of literals. An
// array nested inside an array, and any dynamic expression, has no value form in this slice.
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

// Section 2.2: a record member is a value type, so a `callback` or the `children` slot is not one,
// directly or inside a list/optional wrapper.
void check_member_type(ValidationContext& context, const RecordMember& member,
                       const SourceSpan& where) {
    const std::string_view inner = innermost_type(member.type);
    if (inner == "callback" || inner == "children") {
        context.report("TESSERA_IR_UNSUPPORTED",
                       "record member `" + member.name + "` is a value: `" + std::string(inner) +
                           "` has no record form",
                       where);
    }
}

// Section 2.2: a record may not contain itself, directly or through other records. One back edge is
// reported, at the record that closes it; the DFS follows module order, so it is deterministic.
bool visit_record_type(const ValidationContext& context, std::string_view name,
                       std::unordered_map<std::string, int>& state,
                       std::vector<std::string>& path) {
    constexpr int kOnStack = 1;
    constexpr int kDone = 2;
    int& current = state[std::string(name)];
    if (current == kOnStack) {
        path.emplace_back(name); // Close the loop so `describe_cycle` can render it.
        return true;
    }
    if (current == kDone) {
        return false;
    }
    current = kOnStack;
    path.emplace_back(name);
    const RecordType* type = find_declared_type(context, name);
    if (type != nullptr) {
        for (const RecordMember& member : type->members) {
            const std::string_view target = innermost_type(member.type);
            if (find_declared_type(context, target) == nullptr) {
                continue;
            }
            if (visit_record_type(context, target, state, path)) {
                return true;
            }
        }
    }
    path.pop_back();
    current = kDone;
    return false;
}

// `A -> B -> A` for a path whose last entry repeats the record the loop starts at.
std::string describe_cycle(const std::vector<std::string>& path) {
    std::string chain;
    for (const std::string& step : path) {
        chain += (chain.empty() ? "" : " -> ") + step;
    }
    return chain;
}

} // namespace

void validate_type_names(ValidationContext& context, std::string_view type, const SourceSpan& where,
                         bool mark_used) {
    const std::string_view name = innermost_type(type);
    if (!is_identifier_type(name) || is_primitive_type(name) || name == "callback" ||
        name == "children") {
        return;
    }
    if (is_predeclared_record(name)) {
        return; // A framework payload record: valid, but not a project declaration to track.
    }
    if (find_declared_type(context, name) == nullptr) {
        context.report("TESSERA_IR_UNKNOWN_TYPE", "unknown type `" + std::string(name) + "`",
                       where);
        return;
    }
    if (mark_used) {
        context.used_types.emplace(name);
    }
}

void check_type_table(ValidationContext& context) {
    for (std::size_t i = 0; i < context.module.types.size(); ++i) {
        const RecordType& type = context.module.types[i];
        if (type.name.empty()) {
            context.report("TESSERA_IR_MALFORMED", "a record type has no name", type.source);
        }
        if (!type.name.empty() && type.id != std::string(kTypePrefix) + type.name) {
            context.report("TESSERA_IR_MALFORMED",
                           "record type id `" + type.id + "` must be `type::" + type.name + "`",
                           type.source);
        }
        validate_span(context, type.source, "record type `" + type.name + "`");
        if (i > 0 && !(context.module.types[i - 1].name < type.name)) {
            context.report("TESSERA_IR_ORDER", "record types must be sorted by name", type.source);
        }
        if (type.name.empty() || context.types.count(type.name) != 0) {
            if (!type.name.empty()) {
                context.report("TESSERA_IR_DUPLICATE_ID",
                               "duplicate record type `" + type.name + "`", type.source);
            }
            continue;
        }
        context.types.emplace(type.name, &type);
    }
    // A second pass, so a member may name a record declared later in the table.
    for (const RecordType& type : context.module.types) {
        std::unordered_set<std::string> member_names;
        for (const RecordMember& member : type.members) {
            if (member.name.empty()) {
                context.report("TESSERA_IR_MALFORMED",
                               "a member of record `" + type.name + "` has no name", type.source);
                continue;
            }
            if (!member_names.insert(member.name).second) {
                context.report("TESSERA_IR_DUPLICATE_ID",
                               "duplicate member `" + member.name + "` of record `" + type.name +
                                   "`",
                               type.source);
            }
            // `validate_type` also resolves the name against the table (section 2.2), so a member
            // type is checked once: shape, then the identifier it names. It does not mark the
            // member record used: that follows from the records a component names, transitively
            // (`close_used_types`), so a record reachable only from an unused record is unused too.
            validate_type(context, member.type, type.source, false);
            check_member_type(context, member, type.source);
        }
    }
    // Cycles last, once every member type is syntactically known. A record may not contain itself,
    // directly or through other records: the DFS reports the first back edge at the record that
    // closes it.
    std::unordered_map<std::string, int> state; // 1 = on the stack, 2 = done
    std::vector<std::string> path;
    for (const RecordType& type : context.module.types) {
        if (find_declared_type(context, type.name) == nullptr) {
            continue; // A duplicate: already reported.
        }
        if (visit_record_type(context, type.name, state, path)) {
            context.report("TESSERA_IR_TYPE_CYCLE", "record type cycle: " + describe_cycle(path),
                           type.source);
            return; // One cycle rejects the module; a second report is noise.
        }
    }
}

void close_used_types(ValidationContext& context) {
    // Section 9: "a declared record nothing uses" is a warning. A record a component names is used,
    // and so is every record its members name, transitively; a record reachable only from an unused
    // record is still unused.
    std::vector<std::string> work(context.used_types.begin(), context.used_types.end());
    while (!work.empty()) {
        const std::string name = std::move(work.back());
        work.pop_back();
        const RecordType* type = find_declared_type(context, name);
        if (type == nullptr) {
            continue;
        }
        for (const RecordMember& member : type->members) {
            const std::string_view inner = innermost_type(member.type);
            if (find_declared_type(context, inner) != nullptr &&
                context.used_types.emplace(inner).second) {
                work.emplace_back(inner);
            }
        }
    }
}

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
        const RecordMember* member = nullptr;
        std::size_t index = 0;
        for (std::size_t i = 0; i < type->members.size(); ++i) {
            if (type->members[i].name == field.name) {
                member = &type->members[i];
                index = i;
                break;
            }
        }
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
        // the permitted forms at all, which is the same unsupported-form rule as a dynamic value,
        // not a value/type mismatch.
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
        if (!literal_matches(context, *field.value, member->type)) {
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
        const RecordMember* member = nullptr;
        for (const RecordMember& candidate : type->members) {
            if (candidate.name == segment) {
                member = &candidate;
                break;
            }
        }
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
