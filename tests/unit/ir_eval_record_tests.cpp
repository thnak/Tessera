// SPDX-License-Identifier: MIT
//
// ADR-051 slice 5: record values in the IR evaluator. Split out of `ir_eval_tests.cpp` when that
// file crossed the 500-line hard limit; the group is self-contained (it builds its own expressions
// and needs nothing from the numeric or template groups).

#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/expr.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace ir = tessera::compiler::ir;
namespace eval = tessera::compiler::eval;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ir_eval_record: " << message << "\n";
    }
    return condition;
}

ir::ExprPtr lit(int value) {
    return ir::make_literal(std::int64_t{value});
}

// ADR-051 slice 5: a record value evaluates, and a dotted read resolves through it. The evaluator
// has no type table, so it does not re-check the field set (the IR validator did that against
// `types`); what it must do is produce a value whose fields are in member order and let
// `state.count` read one.
bool records() {
    bool ok = true;

    // `Size{ w = 1, h = 2 }` evaluates to a record with both fields, in the IR's order.
    ir::RecordField w{"w", lit(1)};
    ir::RecordField h{"h", lit(2)};
    std::vector<ir::RecordField> fields;
    fields.push_back(std::move(w));
    fields.push_back(std::move(h));
    const ir::ExprPtr record = ir::make_record("Size", std::move(fields));

    eval::Evaluator evaluator;
    const eval::Scope empty;
    const auto value = evaluator.evaluate(*record, empty);
    ok = expect(value.has_value(), "a record value evaluates") && ok;
    if (!value) {
        return ok;
    }
    const eval::Record* held = value->as_record();
    ok = expect(held != nullptr, "the value is a record") && ok;
    if (held == nullptr) {
        return ok;
    }
    ok = expect(held->type == "Size", "the record keeps its declared type") && ok;
    ok = expect(held->fields.size() == 2, "both fields are present") && ok;
    if (held->fields.size() == 2) {
        ok = expect(held->fields[0].name == "w", "the first field is `w`, in member order") && ok;
        ok = expect(held->fields[1].name == "h", "the second field is `h`, in member order") && ok;
    }
    ok = expect(eval::type_name(*value) == "record", "the type name is `record`") && ok;
    ok = expect(eval::describe(*value) == "Size{w = 1, h = 2}",
                "describe prints the aggregate shape (saw `" + eval::describe(*value) + "`)") &&
         ok;

    // A dotted read resolves a member through the record in scope.
    eval::Scope scope;
    scope["state::size"] = *value;
    const auto member = evaluator.evaluate(*ir::make_ref("state::size.w"), scope);
    ok = expect(member.has_value(), "a dotted read resolves") && ok;
    if (member) {
        const std::int32_t* number = member->as_int();
        ok = expect(number != nullptr && *number == 1, "the dotted read yields the member value") &&
             ok;
    }

    // A member the record does not have is a named diagnostic, not a silent null.
    const auto missing = evaluator.evaluate(*ir::make_ref("state::size.nope"), scope);
    ok = expect(!missing.has_value(), "an unknown member is rejected") && ok;
    if (!missing) {
        ok = expect(missing.error().code == "TESSERA_IR_EVAL_UNKNOWN_REF",
                    "the code is TESSERA_IR_EVAL_UNKNOWN_REF") &&
             ok;
    }

    // Two records are identical only when the type, the field names and the values all match.
    const auto again = evaluator.evaluate(*record, empty);
    ok = expect(again.has_value() && eval::identical(*value, *again),
                "the same record evaluates identically twice") &&
         ok;
    ir::RecordField other_w{"w", lit(9)};
    ir::RecordField other_h{"h", lit(2)};
    std::vector<ir::RecordField> other_fields;
    other_fields.push_back(std::move(other_w));
    other_fields.push_back(std::move(other_h));
    const auto different =
        evaluator.evaluate(*ir::make_record("Size", std::move(other_fields)), empty);
    ok = expect(different.has_value() && !eval::identical(*value, *different),
                "a different field value is not identical") &&
         ok;
    return ok;
}

} // namespace

int main() {
    if (!records()) {
        return 1;
    }
    std::cout << "ir_eval_record: ok\n";
    return 0;
}
