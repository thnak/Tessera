// SPDX-License-Identifier: MIT

// The expression half of the binding-read analysis (ADR-056 slice 1): which cells does one IR
// expression read. See binding_reads.hpp for the conservative rule.

#include "binding_reads.hpp"
#include "binding_reads_collector.hpp"

#include <algorithm>
#include <iterator>
#include <string_view>

namespace tessera::compiler::codegen::internal {
namespace {

// Functions the expression emitter spells (`emit_expr.cpp`) or defers: all are pure over their
// arguments, so a call reads exactly what its arguments read.
bool pure_callee(std::string_view callee) {
    static constexpr std::string_view kPure[] = {
        "min",   "max",   "clamp",   "abs",    "floor",        "ceil",
        "round", "toInt", "toFloat", "length", "formatNumber",
    };
    return std::find(std::begin(kPure), std::end(kPure), callee) != std::end(kPure);
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

} // namespace

ReadCollector::ReadCollector(const ir::Component& component, std::span<const std::string> locals)
    : component_(component),
      locals_(locals.begin(), locals.end()) {}

std::vector<std::string> declared_cells(const ir::Component& component) {
    std::vector<std::string> cells;
    for (const ir::State& state : component.state) {
        cells.push_back(state.id);
    }
    for (const ir::Prop& prop : component.props) {
        cells.push_back("prop::" + prop.name);
    }
    for (const ir::AnimatedValue& anim : component.animated_values) {
        cells.push_back(anim.id);
    }
    for (const ir::Environment& env : component.environment) {
        cells.push_back(env.id);
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
    if (starts_with(base, "state::") || starts_with(base, "prop::") ||
        starts_with(base, "anim::") || starts_with(base, "env::")) {
        const std::vector<std::string> declared = declared_cells(component_);
        if (std::binary_search(declared.begin(), declared.end(), base)) {
            cells_.push_back(base);
            return;
        }
    }
    all_ = true; // not a cell this component declares and not a local: assume everything
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
        for (const ir::ExprPtr& operand : expr->operands) {
            add(operand.get());
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
    set.cells = all_ ? declared_cells(component_) : cells_;
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
