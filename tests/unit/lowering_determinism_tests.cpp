// SPDX-License-Identifier: MIT

// Determinism of expression lowering: the IR is a pure function of the AST and the resolver, so
// lowering twice (and lowering the same AST twice) gives identical serialised JSON
// (docs/design/jsx-ir-lowering.md section 7).

#include "lowering_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace lowering_test;

const MapNames kNames{
    {"a", "state::a"},
    {"b", "state::b"},
    {"c", "state::c"},
    {"count", "state::count"},
    {"ok", "state::ok"},
    {"props.title", "prop::title"},
    {"item.row.id", "item.row.id"},
};

constexpr std::string_view kSources[] = {
    "count",
    "props.title",
    "item.row.id",
    "!ok",
    "a + b * c",
    "a === b",
    "a !== b",
    "c ? a : b",
    "`n=${count}!`",
    "max(min(a, b), count)",
    "formatNumber(count, 2)",
    "a < b && c >= b || ok",
    "`${a}-${formatNumber(count, 2)}`",
};

std::string lower_text(std::string_view source) {
    const Lowered lowered = lower(source, &kNames);
    const ir::Expr* expr = expr_of(lowered);
    return expr != nullptr ? ir_text(*expr) : std::string("<no IR>");
}

void test_two_lowerings_agree() {
    for (const std::string_view source : kSources) {
        const std::string first = lower_text(source);
        const std::string second = lower_text(source);
        check(first != "<no IR>", std::string(source) + " lowers");
        check_eq(first, second, std::string(source) + " is deterministic across calls");
        check(!first.empty() && first.back() == '\n', std::string(source) + " ends with LF");
        check(first.find('\r') == std::string::npos, std::string(source) + " has no CR");
    }
}

void test_same_ast_lowers_twice() {
    const std::string source = "max(min(a, b), count) + (c ? 1 : 2)";
    const jsx::ExpressionParseResult parsed = jsx::parse_expression(source);
    check(parsed.root != nullptr, "the determinism fixture parses");
    if (!parsed.root) {
        return;
    }
    const jsx::SourceMap map(source);
    const lowering::ExpressionLoweringContext context{map, "ui/Counter.tsx", &kNames};
    const lowering::ExpressionLoweringResult first =
        lowering::lower_expression(*parsed.root, context);
    const lowering::ExpressionLoweringResult second =
        lowering::lower_expression(*parsed.root, context);
    check(first.has_value() && second.has_value(), "the same AST lowers twice");
    if (first && second) {
        check_eq(ir_text(*first->get()), ir_text(*second->get()),
                 "lowering does not consume or change the AST");
    }
}

} // namespace

int main() {
    test_two_lowerings_agree();
    test_same_ast_lowers_twice();
    return finish("lowering determinism tests");
}
