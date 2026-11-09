// SPDX-License-Identifier: MIT

#include "jsx_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

// Validity of each item, in order: "VIV" for valid, invalid, valid.
std::string validity(const ModuleParseResult& result) {
    std::string out;
    if (result.root) {
        for (const auto& item : result.root->items) {
            out += item.valid ? 'V' : 'I';
        }
    }
    return out;
}

void test_imports() {
    check_eq(dump_module("import { useState, Text } from \"tessera\";"),
             "(module\n  (import \"tessera\" useState Text))", "named imports");
    check_eq(dump_module("import {} from \"tessera\";"), "(module\n  (import \"tessera\"))",
             "empty import list");
    check_eq(dump_module("import { A, } from \"./A\";"), "(module\n  (import \"./A\" A))",
             "trailing comma, relative path");
    check_eq(dump_module("import \"./theme.css\";"),
             "(module\n  (import-side-effect \"./theme.css\"))", "stylesheet import");
    check(parse_module("import { A } from \"../up/A\";").ok(), "parent-relative path");
}

void test_import_rejections() {
    struct Case {
        const char* source;
        const char* codes;
    };
    const Case cases[] = {
        {"import X from \"tessera\";", "TESSERA_JSX_SUBSET_IMPORT;"},
        {"import * as X from \"tessera\";", "TESSERA_JSX_SUBSET_IMPORT;"},
        {"import { A as B } from \"tessera\";", "TESSERA_JSX_SUBSET_IMPORT;"},
        {"import { A } from \"react\";", "TESSERA_JSX_SUBSET_IMPORT;"},
        {"import { A } from \"/abs/A\";", "TESSERA_JSX_SUBSET_IMPORT;"},
        {"import \"./x.js\";", "TESSERA_JSX_SUBSET_IMPORT;"},
        {"export default function A() {}", "TESSERA_JSX_SUBSET_IMPORT;"},
        {"export { A };", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;"},
        {"import { A } \"tessera\";", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;"},
        {"import { A } from tessera;", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;"},
    };
    for (const auto& test : cases) {
        const auto result = parse_module(test.source);
        check_eq(codes(result), test.codes, std::string("rejection of ") + test.source);
        check_eq(validity(result), "I", std::string("item invalid: ") + test.source);
    }
}

void test_types_and_constants() {
    check_eq(dump_module("export type Props = { initial: int };"),
             "(module\n  (type Props {initial:int}))", "type alias");
    check(parse_module("export type P = int;").root->items[0].exported, "exported flag");
    check_eq(dump_module("const kMax: int = 10;\nexport const kName = \"x\";"),
             "(module\n  (const kMax : int (num 10))\n  (const kName (str \"x\")))", "constants");
    check_eq(codes(parse_module("type Bad = any;")), "TESSERA_JSX_SUBSET_TYPES;",
             "type errors surface from aliases");
    check_eq(codes(parse_module("const a = ;")), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "constant without a value");
}

constexpr std::string_view kCounter =
    "import { useState, useEffect, View, Text, Button } from \"tessera\";\n"
    "\n"
    "type CounterProps = { initial: int };\n"
    "\n"
    "export function Counter({ initial = 0 }: CounterProps) {\n"
    "  const [count, setCount] = useState<int>(initial);\n"
    "  const doubled = count * 2;\n"
    "  useEffect(() => { log(count); }, [count]);\n"
    "  return (\n"
    "    <View>\n"
    "      <Text>Count: {count}</Text>\n"
    "      <Button onClick={() => setCount(count + 1)}>Add</Button>\n"
    "    </View>\n"
    "  );\n"
    "}\n";

void test_component_with_hooks() {
    const auto result = parse_module(kCounter);
    check(result.ok(), "the counter parses cleanly: " + codes(result));
    check_eq(dump(*result.root),
             "(module\n"
             "  (import \"tessera\" useState useEffect View Text Button)\n"
             "  (type CounterProps {initial:int})\n"
             "  (export-component Counter (props {initial=(num 0)} : CounterProps) (body"
             " (const [count setCount] (call (id useState) <int> (id initial)))"
             " (const doubled (binary * (id count) (num 2)))"
             " (expr (call (id useEffect) (arrow () (block (call (id log) (id count))))"
             " (array (id count))))) (return (element View (element Text (text \"Count: \")"
             " (expr (id count))) (element Button (attr onClick (arrow () (call (id setCount)"
             " (binary + (id count) (num 1))))) (text \"Add\"))))))",
             "full module dump");
}

void test_component_shapes() {
    check_eq(dump_module("function Hello() { return <Text>Hi</Text>; }"),
             "(module\n  (component Hello (body) (return (element Text (text \"Hi\")))))",
             "no parameter, no statements, no parentheses around the element");
    check_eq(dump_module("function A(props: P): Node { return (<B/>); }"),
             "(module\n  (component A (props props : P) (returns Node) (body)"
             " (return (element B/))))",
             "named props, return type, parenthesized return");
    check_eq(dump_module("function A({ a, b = \"x\", c = true, d = null, }: P) { return <B/>; }"),
             "(module\n  (component A (props {a,b=(str \"x\"),c=(true),d=(null)} : P) (body)"
             " (return (element B/))))",
             "destructured props with literal defaults");
    check_eq(dump_module("function A() {\n  const r = useRef<Text>(null);\n  return <B/>;\n}"),
             "(module\n  (component A (body (const r (call (id useRef) <Text> (null))))"
             " (return (element B/))))",
             "ref hook");
    check_eq(
        dump_module("function A() {\n  const v: int = useAnimatedValue(0);\n  return <B/>;\n}"),
        "(module\n  (component A (body (const v : int (call (id useAnimatedValue) (num 0))))"
        " (return (element B/))))",
        "annotated local");
}

void test_item_and_statement_spans() {
    const std::string source(kCounter);
    const auto result = parse_module(source);
    if (!result.root || result.root->items.size() != 3) {
        check(false, "three items");
        return;
    }
    const SourceMap map(source);
    const auto& items = result.root->items;
    check_eq(std::string(map.slice(items[0].span)),
             "import { useState, useEffect, View, Text, Button } from \"tessera\";", "import span");
    check_eq(std::string(map.slice(items[1].span)), "type CounterProps = { initial: int };",
             "type alias span");
    check(map.slice(items[2].span).starts_with("export function Counter"), "component start");
    check(map.slice(items[2].span).ends_with("}"), "component end");
    check_eq(std::string(map.slice(items[2].body[0].span)),
             "const [count, setCount] = useState<int>(initial);", "const statement span");
    check_eq(std::string(map.slice(items[2].body[2].span)),
             "useEffect(() => { log(count); }, [count]);", "expression statement span");
    check_eq(std::string(map.slice(items[2].name_span)), "Counter", "name span");
    check_eq(std::string(map.slice(items[2].returned->span)).substr(0, 5), "<View", "element span");
}

void test_comments_keep_their_position() {
    const std::string source =
        "function A() {\n  // Note: bind::on_x\n  const f = 1;\n  return <B/>;\n}\n";
    const auto result = parse_module(source);
    check(result.ok() && result.comments.size() == 1, "one comment, no errors");
    if (!result.ok() || result.comments.size() != 1) {
        return;
    }
    const auto& statement = result.root->items[0].body[0];
    check(result.comments[0].span.end() <= statement.span.offset,
          "the comment sits before the statement it annotates, so a later pass can pair them");
    check_eq(std::string(result.comments[0].text), " Note: bind::on_x", "comment text");
}

void test_component_rejections() {
    struct Case {
        const char* source;
        const char* codes;
    };
    const Case cases[] = {
        {"function a() { return <B/>; }", "TESSERA_JSX_SUBSET_COMPONENT_NAME;"},
        {"function useThing() { return <B/>; }", "TESSERA_REACT_CUSTOM_HOOK;"},
        {"function A() { const a = 1; }", "TESSERA_JSX_PARSE_MISSING_RETURN;"},
        {
            "function A() { return <B/>; const a = 1; }",
            "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
        },
        {"function A() { return null; }", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;"},
        {"function A() { return \"x\"; }", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;"},
        {"function A() { return; }", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;"},
        {"function A(props) { return <B/>; }", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;"},
        {"function A({ a }) { return <B/>; }", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;"},
        {"function A({ ...r }: P) { return <B/>; }", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;"},
        {"function A({ a = b }: P) { return <B/>; }", "TESSERA_JSX_SUBSET_LITERAL;"},
        {"function A() { if (a) {} return <B/>; }", "TESSERA_JSX_SUBSET_STATEMENT;"},
        {"function A() { let x = 1; return <B/>; }", "TESSERA_JSX_SUBSET_MUTATION;"},
        {
            "function A() { function f() {} return <B/>; }",
            "TESSERA_JSX_SUBSET_FUNCTION_VALUE;",
        },
        {
            "function A() { const {a} = b; return <B/>; }",
            "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
        },
        {"function A() { x = 1; return <B/>; }", "TESSERA_JSX_SUBSET_MUTATION;"},
        {"function A() { return <B/> }", "TESSERA_JSX_PARSE_MISSING_SEMICOLON;"},
        {"function A() { f() return <B/>; }", "TESSERA_JSX_PARSE_MISSING_SEMICOLON;"},
        {"class A {}", "TESSERA_JSX_SUBSET_CLASS;"},
        {"foo();", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;"},
        {"if (a) {}", "TESSERA_JSX_SUBSET_STATEMENT;"},
    };
    for (const auto& test : cases) {
        const auto result = parse_module(test.source);
        check_eq(codes(result), test.codes, std::string("rejection of ") + test.source);
        check_eq(validity(result), "I", std::string("item is invalid: ") + test.source);
    }
    const auto semicolon = parse_module("const a = 1\nconst b = 2;");
    check_eq(codes(semicolon), "TESSERA_JSX_PARSE_MISSING_SEMICOLON;", "missing semicolon");
    check_eq(format_diagnostic(first_diag(semicolon)),
             "1:12: error[TESSERA_JSX_PARSE_MISSING_SEMICOLON]: missing `;`\n  help: insert `;`",
             "reported at the end of the previous token");
    check_eq(validity(semicolon), "IV", "the next item is unaffected");
}

void test_recovery_keeps_other_items() {
    const std::string source = "function A() { return <A/>; }\n"
                               "function Broken() {\n"
                               "  const [a = useState(;\n"
                               "  return <X/>;\n"
                               "}\n"
                               "function C() { return <C/>; }\n";
    const auto result = parse_module(source);
    check_eq(validity(result), "VIV", "only the broken component is invalid");
    check_eq(codes(result), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "one diagnostic, no cascade");
    check_eq(format_diagnostic(first_diag(result)),
             "3:12: error[TESSERA_JSX_PARSE_UNEXPECTED_TOKEN]: unexpected `=`, expected `]`",
             "diagnostic names the first real problem");
    check(result.root && result.root->items.size() == 3, "all items are present");
    if (result.root && result.root->items.size() == 3) {
        check_eq(result.root->items[1].name, "Broken", "the invalid item keeps its name");
        check_eq(dump(*result.root->items[2].returned), "(element C/)", "later item fully parsed");
        const SourceMap map(source);
        check(map.slice(result.root->items[1].span).starts_with("function Broken"),
              "invalid item span starts at the item");
    }
}

void test_recovery_rules() {
    // An unclosed `{`: the next column-1 item keyword still starts a new item.
    const auto unclosed =
        parse_module("function A() { return <A/>;\nfunction B() { return <B/>; }\n");
    check_eq(validity(unclosed), "IV", "unclosed brace does not swallow the next item");

    // Indented keywords inside open braces are not item starts.
    const auto nested = parse_module("function A() {\n"
                                     "  const x = 1 +;\n"
                                     "  const y = {\n"
                                     "    function inner() {}\n"
                                     "  };\n"
                                     "  return <A/>;\n"
                                     "}\n"
                                     "function B() { return <B/>; }\n");
    check_eq(validity(nested), "IV", "only the real item boundary resumes parsing");
    check_eq(codes(nested), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "a single diagnostic");

    // Lexer problems inside the skipped region are not reported.
    const auto noisy =
        parse_module("function A() { const = ; return <Text>don't \"quote</Text>; }\n"
                     "function B() { return <B/>; }\n");
    check_eq(codes(noisy), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "skipped text produces no lexer diagnostics");
    check_eq(validity(noisy), "IV", "B recovered");

    // Garbage at the top level makes progress and does not hide the next item.
    check_eq(validity(parse_module("foo();\nconst a = 1;")), "IV", "stray statement");
    check_eq(validity(parse_module("}\nconst a = 1;")), "IV", "stray brace");

    // Every bad item reports once.
    const auto many = parse_module("const = 1;\nconst = 2;\nconst = 3;\nconst ok = 4;\n");
    check_eq(validity(many), "IIIV", "three bad, one good");
    check(many.diagnostics.size() == 3, "one diagnostic per bad item");

    // An error in one component does not change another component's tree.
    const std::string good = "function G() { const [n, setN] = useState<int>(0); return <T/>; }\n";
    const auto alone = parse_module(good);
    const auto after = parse_module("function X() { const = ; }\n" + good);
    check(after.root && after.root->items.size() == 2 && alone.root, "two items");
    if (after.root && after.root->items.size() == 2 && alone.root) {
        Module only_second;
        only_second.items.push_back(std::move(after.root->items[1]));
        check_eq(dump(only_second), dump(*alone.root), "isolation between components");
    }
}

void test_limits_and_edge_files() {
    Limits few;
    few.max_components = 2;
    const auto result =
        parse_module("function A() { return <A/>; }\nfunction B() { return <B/>; }\n"
                     "function C() { return <C/>; }\nfunction D() { return <D/>; }\n",
                     few);
    check_eq(codes(result), "TESSERA_JSX_LIMIT_COMPONENTS;", "component limit");
    check(result.root && result.root->items.size() == 2, "items before the limit are kept");

    check(parse_module("").ok() && parse_module("").root->items.empty(), "empty file");
    check(parse_module("// only a comment\n").ok(), "comment-only file");
    check(parse_module("\xEF\xBB\xBF"
                       "const a = 1;")
              .ok(),
          "BOM");
    Limits tiny;
    tiny.max_file_bytes = 4;
    const auto big = parse_module("const a = 1;", tiny);
    check_eq(codes(big), "TESSERA_JSX_LIMIT_FILE_SIZE;", "file size limit");
    check(big.root == nullptr, "no module for an unreadable file");
}

void test_determinism() {
    const std::string source = std::string(kCounter) + "function Broken() { const = ; }\n";
    check_eq(dump_module(source), dump_module(source), "same dump");
    check_eq(codes(parse_module(source)), codes(parse_module(source)), "same diagnostics");
}

} // namespace

int main() {
    test_imports();
    test_import_rejections();
    test_types_and_constants();
    test_component_with_hooks();
    test_component_shapes();
    test_item_and_statement_spans();
    test_comments_keep_their_position();
    test_component_rejections();
    test_recovery_keeps_other_items();
    test_recovery_rules();
    test_limits_and_edge_files();
    test_determinism();
    return finish("jsx module tests");
}
