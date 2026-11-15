// SPDX-License-Identifier: MIT

// Record type table checks (docs/specs/ir-v1.md sections 2.2 and 11; ADR-051): ids, names, sorted
// order, spans, member types and cycles, plus the transitive "used" closure the unused-record
// warning needs. The record value rules are in validate_record_value.cpp.

#include "validate_record_detail.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tessera::compiler::ir {
namespace {

using record_detail::find_declared_type;
using record_detail::innermost_type;

constexpr std::string_view kTypePrefix = "type::";

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
    if (!record_detail::is_identifier_type(name) || record_detail::is_primitive_type(name) ||
        name == "callback" || name == "children") {
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

} // namespace tessera::compiler::ir
