// SPDX-License-Identifier: MIT

// ADR-066 item 1: the cheap acceptances. Split from jsx_module_tests.cpp when that file reached the
// 500-line hard limit; the subject is the React spellings a port uses, not the module grammar.

#include "jsx_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

// ADR-066 item 1: the cheap acceptances. Each case is the spelling a ported React file uses, and
// the check is that it parses as the item it should be rather than as an error.
void test_react_core_acceptances() {
    // `import React from "react"` and `import * as React from "react"`: accepted and ignored,
    // because the automatic JSX runtime means the binding is never read.
    const auto default_import = parse_module("import React from \"react\";");
    check_eq(codes(default_import), "", "`import React from \"react\"` is accepted");
    if (default_import.root && default_import.root->items.size() == 1) {
        check(default_import.root->items[0].ignored_react_import,
              "the React default import is marked ignored");
    } else {
        check(false, "one item for the React default import");
    }
    const auto namespace_import = parse_module("import * as React from \"react\";");
    check_eq(codes(namespace_import), "", "`import * as React from \"react\"` is accepted");
    if (namespace_import.root && namespace_import.root->items.size() == 1) {
        check(namespace_import.root->items[0].ignored_react_import,
              "the React namespace import is marked ignored");
    } else {
        check(false, "one item for the React namespace import");
    }
    // Any other default or namespace import stays the rejection it was.
    check_eq(codes(parse_module("import React from \"./React\";")), "TESSERA_JSX_SUBSET_IMPORT;",
             "a non-React default import is still rejected");
    check_eq(codes(parse_module("import * as R from \"react\";")), "TESSERA_JSX_SUBSET_IMPORT;",
             "a differently named namespace import is still rejected");

    // `export default function Name(...)`: the component is declared under its own name.
    const auto default_export = parse_module("export default function Page() { return <div />; }");
    check_eq(codes(default_export), "", "`export default function` is accepted");
    if (default_export.root && default_export.root->items.size() == 1) {
        const auto& item = default_export.root->items[0];
        check(item.kind == ItemKind::Component, "the default export is a component");
        check(item.default_export, "the item records that it was the default export");
        check_eq(item.name, "Page", "the component keeps its declared name");
    } else {
        check(false, "one item for the default-exported component");
    }

    // Arrow-function components, in both body spellings.
    const auto arrow_block =
        parse_module("export const Avatar = ({ initials }: { initials: string }) => {\n"
                     "  return <div>{initials}</div>;\n"
                     "};");
    check_eq(codes(arrow_block), "", "an arrow component with a block body is accepted");
    if (arrow_block.root && arrow_block.root->items.size() == 1) {
        const auto& item = arrow_block.root->items[0];
        check(item.kind == ItemKind::Component, "the arrow is a component");
        check_eq(item.name, "Avatar", "the arrow component keeps its name");
        check(item.returned != nullptr, "the arrow component has a returned element");
        check(item.has_props && item.props_destructured,
              "the arrow component has destructured props");
    } else {
        check(false, "one item for the arrow component");
    }
    const auto arrow_expr = parse_module("const Badge = () => <span />;");
    check_eq(codes(arrow_expr), "", "an arrow component with an expression body is accepted");
    if (arrow_expr.root && arrow_expr.root->items.size() == 1) {
        check(arrow_expr.root->items[0].kind == ItemKind::Component,
              "the expression-bodied arrow is a component");
        check(arrow_expr.root->items[0].returned != nullptr,
              "the expression-bodied arrow has a returned element");
    } else {
        check(false, "one item for the expression-bodied arrow");
    }

    // A lowercase `const` arrow is an ordinary constant, not a component: the attempt is rewound
    // and the text parses as the constant it is.
    const auto helper = parse_module("const format = (v: string) => v;");
    check_eq(codes(helper), "", "a lowercase arrow constant is accepted");
    if (helper.root && helper.root->items.size() == 1) {
        check(helper.root->items[0].kind == ItemKind::Const,
              "a lowercase arrow stays a constant, not a component");
        check(helper.root->items[0].init != nullptr, "the constant keeps its initializer");
    } else {
        check(false, "one item for the lowercase arrow constant");
    }
    // An uppercase `const` that is not a component is still a constant.
    const auto not_a_component = parse_module("const LIMIT = 10;");
    check_eq(codes(not_a_component), "", "an uppercase constant is accepted");
    if (not_a_component.root && not_a_component.root->items.size() == 1) {
        check(not_a_component.root->items[0].kind == ItemKind::Const,
              "an uppercase constant stays a constant");
    } else {
        check(false, "one item for the uppercase constant");
    }

    // `useMemo` and `useCallback`, safe forms: the wrapped value becomes the initializer.
    const auto memo = parse_module("function A({ rows }: { rows: string[] }) {\n"
                                   "  const n = useMemo(() => rows.length, [rows]);\n"
                                   "  return <div>{n}</div>;\n"
                                   "}");
    check_eq(codes(memo), "", "the safe `useMemo` form is accepted");
    if (memo.root && memo.root->items.size() == 1 && !memo.root->items[0].body.empty()) {
        const auto& statement = memo.root->items[0].body[0];
        check(statement.init != nullptr, "the memo statement has an initializer");
        if (statement.init) {
            check(statement.init->kind != ExprKind::Call,
                  "the memo call is desugared away, leaving the wrapped expression");
        }
    } else {
        check(false, "one statement for the memo");
    }
    const auto callback = parse_module("function A() {\n"
                                       "  const [on, setOn] = useState<boolean>(false);\n"
                                       "  const toggle = useCallback(() => setOn(!on), [on]);\n"
                                       "  return <div onClick={toggle} />;\n"
                                       "}");
    check_eq(codes(callback), "", "the safe `useCallback` form is accepted");
    if (callback.root && callback.root->items.size() == 1 &&
        callback.root->items[0].body.size() > 1) {
        const auto& statement = callback.root->items[0].body[1];
        check(statement.init != nullptr && statement.init->kind == ExprKind::Arrow,
              "the callback call is desugared to the arrow it wraps");
    } else {
        check(false, "two statements for the callback");
    }
    // A form outside the safe shapes is left alone, so the rules report it.
    const auto unsafe_memo = parse_module(
        "function A() { const v = useMemo(() => { return 1; }, []); return <div>{v}</div>; }");
    check_eq(codes(unsafe_memo), "", "an unsafe `useMemo` still parses");
    if (unsafe_memo.root && unsafe_memo.root->items.size() == 1 &&
        !unsafe_memo.root->items[0].body.empty()) {
        const auto& statement = unsafe_memo.root->items[0].body[0];
        check(statement.init != nullptr && statement.init->kind == ExprKind::Call,
              "an unsafe `useMemo` keeps its call, so the rules can reject it");
    } else {
        check(false, "one statement for the unsafe memo");
    }

    // `??` and `?.` are expressions now, not operator rejections.
    check_eq(codes(parse_module("function A({ a, b }: { a?: string; b: string }) {"
                                " return <div>{a ?? b}</div>; }")),
             "", "`??` is accepted");
    check_eq(codes(parse_module("function A({ a }: { a?: string }) {"
                                " return <div>{a?.length}</div>; }")),
             "", "`?.` is accepted");
    // The other rejected operators keep their codes.
    check_eq(codes(parse_module("function A({ a }: { a: int }) { return <div>{a ** 2}</div>; }")),
             "TESSERA_JSX_SUBSET_OPERATOR;", "`**` is still rejected");
}
} // namespace

int main() {
    test_react_core_acceptances();
    return finish("jsx react core acceptance tests");
}