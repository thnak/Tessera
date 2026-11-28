// SPDX-License-Identifier: MIT

// The expression half of the binding-read analysis (ADR-056 slice 1): which cells does one IR
// expression read. See binding_reads.hpp for the conservative rule.

#include "binding_reads.hpp"
#include "binding_reads_collector.hpp"

#include <algorithm>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

// Functions the expression emitter spells (`emit_expr.cpp`) or defers: all are pure over their
// arguments, so a call reads exactly what its arguments read. The six string methods of ADR-066
// item 3 are total and locale-independent (`tessera/ui/scene/list_ops.hpp`), so a derived list
// source that filters with `startsWith` reads its predicate's cells and nothing else; without them
// here the analysis would fall back to "every cell" and recompute the source cache needlessly.
bool pure_callee(std::string_view callee) {
    static constexpr std::string_view kPure[] = {
        "min",         "max",         "clamp",      "abs",    "floor",        "ceil",
        "round",       "toInt",       "toFloat",    "length", "formatNumber", "trim",
        "toUpperCase", "toLowerCase", "startsWith", "slice",  "toFixed",
    };
    return std::find(std::begin(kPure), std::end(kPure), callee) != std::end(kPure);
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

} // namespace

namespace {

// The declared IR type of a cell base (`state::x`, `prop::x`, `anim::x`, `env::x`), or an empty
// view when the component declares no such cell.
std::string_view cell_type(const ir::Component& component, std::string_view base) {
    for (const ir::State& state : component.state) {
        if (state.id == base) {
            return state.type;
        }
    }
    for (const ir::Prop& prop : component.props) {
        if ("prop::" + prop.name == base) {
            return prop.type;
        }
    }
    for (const ir::AnimatedValue& anim : component.animated_values) {
        if (anim.id == base) {
            return anim.type;
        }
    }
    for (const ir::Environment& env : component.environment) {
        if (env.id == base) {
            return env.type;
        }
    }
    return {};
}

// The member `name` of record `type_name` in the table, or null.
const ir::RecordMember* find_record_member(std::span<const ir::RecordType> records,
                                           std::string_view type_name, std::string_view name) {
    for (const ir::RecordType& record : records) {
        if (record.name != type_name) {
            continue;
        }
        for (const ir::RecordMember& member : record.members) {
            if (member.name == name) {
                return &member;
            }
        }
        return nullptr;
    }
    return nullptr;
}

// True when `type` names a declared record (an exact name, not a `list`/`optional` wrapper).
bool is_declared_record(std::string_view type, std::span<const ir::RecordType> records) {
    for (const ir::RecordType& record : records) {
        if (record.name == type) {
            return true;
        }
    }
    return false;
}

// The inner type of `list<T>`/`optional<T>`, or empty.
std::string_view wrapper_inner(std::string_view type, std::string_view wrapper) {
    if (type.size() > wrapper.size() + 1 && type.rfind(wrapper, 0) == 0 && type.back() == '>') {
        return type.substr(wrapper.size(), type.size() - wrapper.size() - 1);
    }
    return {};
}

// Appends the leaf bit ids below `prefix` for a value of `ir_type`: a plain record recurses into
// every member; a `list`/`optional` wrapper is one leaf at `prefix`, because the generated
// expression emitter spells `state_opt_.field` (which would not compile on a `std::optional`) and
// cannot index a list, so no binding names a bit below a wrapper. The setter compares a wrapper
// member whole. `prefix` is the already-joined member path (`state::form.name`).
void collect_leaf_bits(std::span<const ir::RecordType> records, std::string_view ir_type,
                       const std::string& prefix, std::vector<std::string>& out) {
    if (!is_declared_record(ir_type, records)) {
        out.push_back(prefix);
        return;
    }
    for (const ir::RecordType& record : records) {
        if (record.name != ir_type) {
            continue;
        }
        for (const ir::RecordMember& member : record.members) {
            collect_leaf_bits(records,
                              member.optional ? "optional<" + member.type + ">" : member.type,
                              prefix + "." + member.name, out);
        }
        break;
    }
}

} // namespace

ReadCollector::ReadCollector(const ir::Component& component, std::span<const std::string> locals,
                             std::span<const ir::RecordType> records)
    : component_(component),
      locals_(locals.begin(), locals.end()),
      records_(records) {}

std::vector<std::string> cell_bit_ids(std::string_view cell_id, std::string_view ir_type,
                                      std::span<const ir::RecordType> records) {
    if (!is_declared_record(ir_type, records)) {
        return {std::string(cell_id)};
    }
    std::vector<std::string> bits;
    collect_leaf_bits(records, ir_type, std::string(cell_id), bits);
    if (bits.empty()) {
        bits.push_back(std::string(cell_id));
    }
    std::sort(bits.begin(), bits.end());
    bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
    return bits;
}

// The leaf bits under `cell_id` for the member path `rest` (`name`, `address.city`). A path that
// does not resolve to a member falls back to the whole-cell bit, the safe direction.
[[maybe_unused]] std::vector<std::string> member_bit_ids(const ir::Component& component,
                                                         std::span<const ir::RecordType> records,
                                                         std::string_view cell_id,
                                                         std::string_view rest) {
    // Resolve each segment down to the member the read stops at, then expand that member. A dotted
    // read into a non-record member is not a form v1 emits, so it falls back to the whole cell.
    std::string_view type = cell_type(component, cell_id);
    const ir::RecordMember* member = nullptr;
    std::string path;
    std::string_view segment_rest = rest;
    while (!segment_rest.empty()) {
        const std::size_t next = segment_rest.find('.');
        const std::string_view name =
            next == std::string_view::npos ? segment_rest : segment_rest.substr(0, next);
        if (!is_declared_record(type, records)) {
            return {std::string(cell_id)};
        }
        member = find_record_member(records, type, name);
        if (member == nullptr) {
            return {std::string(cell_id)};
        }
        path += "." + std::string(name);
        // The next segment reads a field of this member's record. An `optional<Record>` member is
        // unwrapped once, so `state.form.address.city` walks through `address?: Address`.
        if (const std::string_view inner = wrapper_inner(member->type, "optional<");
            !inner.empty()) {
            type = inner;
        } else {
            type = member->type;
        }
        if (next == std::string_view::npos) {
            break;
        }
        segment_rest = segment_rest.substr(next + 1);
    }
    if (member == nullptr) {
        return {std::string(cell_id)};
    }
    // A read that stops at a wrapper member (`optional<T>`/`list<T>`) names no bit the generated
    // C++ could read (`std::optional` has no `.field`), so it depends on the whole cell rather than
    // a leaf that does not exist.
    if (const std::string inner =
            member->optional ? "optional<" + member->type + ">" : member->type;
        !wrapper_inner(inner, "optional<").empty() || !wrapper_inner(inner, "list<").empty()) {
        return {std::string(cell_id)};
    }
    std::vector<std::string> bits;
    collect_leaf_bits(records, member->type, std::string(cell_id) + path, bits);
    if (bits.empty()) {
        bits.push_back(std::string(cell_id));
    }
    std::sort(bits.begin(), bits.end());
    bits.erase(std::unique(bits.begin(), bits.end()), bits.end());
    return bits;
}

void ReadCollector::add_bits(std::string_view id) {
    const std::string_view base = id.substr(0, id.find('.'));
    const std::string_view rest = id.size() > base.size() ? id.substr(base.size() + 1) : "";
    if (rest.empty()) {
        // A whole-cell read: with the record table, every leaf bit under a record-typed cell, so a
        // change to any field of the record re-runs the binding. Without the table, the cell only.
        const std::string cell_id(base);
        for (const std::string& bit :
             cell_bit_ids(cell_id, cell_type(component_, cell_id), records_)) {
            cells_.push_back(bit);
        }
        return;
    }
    for (const std::string& bit : member_bit_ids(component_, records_, base, rest)) {
        cells_.push_back(bit);
    }
}

bool has_pending_cell(const ir::Component& component) {
    if (!component.resources.empty()) {
        return true;
    }
    for (const ir::Behavior& behavior : component.behaviors) {
        if (behavior.behavior == "transition" || behavior.behavior == "deferred_value") {
            return true;
        }
    }
    for (const ir::Handler& handler : component.handlers) {
        for (const ir::Action& action : handler.actions) {
            if (action.transition) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> declared_cells(const ir::Component& component,
                                        std::span<const ir::RecordType> records) {
    std::vector<std::string> cells;
    const auto add = [&](std::string_view id, std::string_view ir_type) {
        for (const std::string& bit : cell_bit_ids(id, ir_type, records)) {
            cells.push_back(bit);
        }
    };
    for (const ir::State& state : component.state) {
        add(state.id, state.type);
    }
    for (const ir::Prop& prop : component.props) {
        add("prop::" + prop.name, prop.type);
    }
    for (const ir::AnimatedValue& anim : component.animated_values) {
        add(anim.id, anim.type);
    }
    for (const ir::Environment& env : component.environment) {
        add(env.id, env.type);
    }
    if (has_pending_cell(component)) {
        cells.emplace_back(kPendingCell);
    }
    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    return cells;
}

void ReadCollector::add_ref_id(const std::string& id) {
    const std::string base = id.substr(0, id.find('.'));
    if (std::find(locals_.begin(), locals_.end(), base) != locals_.end()) {
        return; // a loop local: the template's list cell is carried by its ListSource binding
    }
    if (starts_with(base, "ref::")) {
        return; // a node ref is assigned once at mount, never a reactive input
    }
    if (starts_with(base, "res::") && has_pending_cell(component_)) {
        // A resource read changes when the model publishes a new state (pending-ui-v1.md 6.1).
        cells_.emplace_back(kPendingCell);
        return;
    }
    const bool namespace_cell = starts_with(base, "state::") || starts_with(base, "prop::") ||
                                starts_with(base, "anim::") || starts_with(base, "env::");
    if (!namespace_cell || cell_type(component_, base).empty()) {
        all_ = true; // not a cell this component declares and not a local: assume everything
        return;
    }
    // A declared cell: record every leaf bit the read touches. A bare `state::form` records every
    // leaf under the record; a dotted `state::form.name` only the field's leaves. A dotted path
    // that the table cannot resolve falls back to the whole cell, the safe direction.
    if (records_.empty() || !is_declared_record(cell_type(component_, base), records_)) {
        cells_.push_back(base);
        return;
    }
    add_bits(id);
}

void ReadCollector::add(const ir::Expr* expr) {
    if (expr == nullptr) {
        all_ = true;
        return;
    }
    switch (expr->kind) {
    case ir::ExprKind::Literal:
        return;
    case ir::ExprKind::Ref:
        add_ref_id(expr->id);
        return;
    case ir::ExprKind::Binary:
    case ir::ExprKind::Unary:
    case ir::ExprKind::Conditional:
    case ir::ExprKind::Array:
    // ADR-066 item 1: `a?.b` reads its base, `a ?? b` reads both sides. Both are pure, so the read
    // set is the union of their operands and nothing else.
    case ir::ExprKind::OptionalField:
    case ir::ExprKind::Coalesce:
    // ADR-066 item 3: `list.slice` reads the list and the bounds.
    case ir::ExprKind::ListSlice:
        for (const ir::ExprPtr& operand : expr->operands) {
            add(operand.get());
        }
        return;
    case ir::ExprKind::ListFilter: {
        // ADR-066 item 3: `list.filter(item => <bool>)` reads the list and whatever the predicate
        // reads. The predicate's own item name is a local, not a cell (the same rule
        // `add_ref_id` applies to a loop local); without binding it here, `row.startsWith(query)`
        // would look like an undeclared base and widen the read set to every cell, which would
        // make a derived list source recompute on any unrelated state change (issue #830).
        if (expr->operands.size() != 2) {
            all_ = true;
            return;
        }
        add(expr->operands[0].get());
        const bool had_item = !expr->id.empty();
        if (had_item) {
            locals_.push_back(expr->id);
        }
        add(expr->operands[1].get());
        if (had_item) {
            locals_.pop_back();
        }
        return;
    }
    case ir::ExprKind::RecordUpdate:
        add_ref_id(expr->id);
        for (const ir::RecordField& field : expr->fields) {
            add(field.value.get());
        }
        return;
    case ir::ExprKind::Call:
        if (!pure_callee(expr->id)) {
            all_ = true;
        }
        for (const ir::ExprPtr& operand : expr->operands) {
            add(operand.get());
        }
        return;
    case ir::ExprKind::Template:
        for (const ir::TemplatePart& part : expr->parts) {
            if (part.expr) {
                add(part.expr.get());
            }
        }
        return;
    case ir::ExprKind::Record:
        for (const ir::RecordField& field : expr->fields) {
            add(field.value.get());
        }
        return;
    }
    all_ = true; // an ExprKind this analysis does not know
}

ReadSet ReadCollector::finish() const {
    ReadSet set;
    set.all = all_;
    set.cells = all_ ? declared_cells(component_, records_) : cells_;
    std::sort(set.cells.begin(), set.cells.end());
    set.cells.erase(std::unique(set.cells.begin(), set.cells.end()), set.cells.end());
    return set;
}

ReadSet expression_reads(const ir::Expr& expr, const ir::Component& component,
                         std::span<const std::string> locals) {
    ReadCollector collector(component, locals);
    collector.add(&expr);
    return collector.finish();
}

} // namespace tessera::compiler::codegen::internal
