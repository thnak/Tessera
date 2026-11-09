// SPDX-License-Identifier: MIT

// `// Note: bind::<id>` annotations: pairing, id rules, positions (spec 7, 9.3, 12).

#include "jsx_test_util.hpp"

#include <string>
#include <string_view>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

std::string component(std::string_view body, std::string_view name = "A") {
    return "function " + std::string(name) + "() {\n" + std::string(body) + "  return <B/>;\n}\n";
}

void test_handlers_and_effects_are_paired() {
    const std::string source =
        component("  // Note: bind::on_pick\n"
                  "  const pick = (id: int): void => { setN(id); };\n"
                  "  // Note: bind::sync\n"
                  "  useEffect(() => { go(); return () => { stop(); }; }, [n]);\n"
                  "  const plain = () => go();\n");
    const auto result = parse_module(source);
    check(result.ok(), "parses cleanly: " + codes(result));
    check_eq(dump(*result.root),
             "(module\n  (component A (body"
             " (const @on_pick pick (arrow (id:int) -> void (block (call (id setN) (id id)))))"
             " (expr @sync (call (id useEffect) (arrow () (block (call (id go))"
             " (return (arrow () (block (call (id stop))))))) (array (id n))))"
             " (const plain (arrow () (call (id go))))) (return (element B/))))",
             "annotations land on the statement after them, others stay unannotated");
    const auto& body = result.root->items[0].body;
    check_eq(std::string(SourceMap(source).slice(body[0].bind_span)), "// Note: bind::on_pick",
             "bind_span covers the whole comment");
    check(body[2].bind_id.empty(), "an unannotated handler has no id");
}

void test_accepted_comment_spellings() {
    const auto tight = parse_module(component("  //Note:bind::x\n  const f = () => go();\n"));
    check(tight.ok() && tight.root->items[0].body[0].bind_id == "x", "no spaces needed");
    const auto padded =
        parse_module(component("  //   Note:   bind::x_1   \n  const f = () => go();\n"));
    check(padded.ok() && padded.root->items[0].body[0].bind_id == "x_1",
          "extra spaces around the parts are fine");
    const auto crlf =
        parse_module("function A() {\r\n  // Note: bind::x\r\n  const f = () => go();\r\n"
                     "  return <B/>;\r\n}\r\n");
    check(crlf.ok() && crlf.root->items[0].body[0].bind_id == "x", "CRLF lines");
}

void test_other_comments_are_ignored() {
    for (const char* comment : {"// Note: something else", "// bind::x", "/* Note: bind::x */",
                                "// note: bind::x", "// Note: bind:x", "// TODO Note: bind::x"}) {
        const auto result =
            parse_module(component(std::string("  ") + comment + "\n  const f = () => go();\n"));
        check(result.ok() && result.root->items[0].body[0].bind_id.empty(),
              std::string("not an annotation: ") + comment + " " + codes(result));
    }
}

void test_id_rules() {
    for (const char* id : {"Foo", "1x", "", "a b", "on-click", "a.b", "_x", "x$"}) {
        const auto result = parse_module(
            component(std::string("  // Note: bind::") + id + "\n  const f = () => go();\n"));
        check_eq(codes(result), "TESSERA_REACT_BIND_ID;", std::string("malformed id `") + id + "`");
        check_eq(first_diag(result).suggestion, "use `[a-z][a-z0-9_]*`", "suggestion");
        check(!result.root->items[0].valid, "the component is invalid");
    }
    check(parse_module(component("  // Note: bind::a_b2\n  const f = () => go();\n")).ok(),
          "lowercase, digits and underscore");
}

void test_duplicates_and_cleanup_reservation() {
    const std::string two = component("  // Note: bind::x\n  const f = () => go();\n", "A") +
                            component("  // Note: bind::x\n  const g = () => go();\n", "C");
    const auto duplicate = parse_module(two);
    check_eq(codes(duplicate), "TESSERA_REACT_BIND_ID;", "same id in two components");
    check_eq(first_diag(duplicate).message, "duplicate bind id `x`", "message");
    check_eq(validity_text(duplicate), "VI", "only the second component is invalid");

    const std::string effect_first =
        component("  // Note: bind::sync\n  useEffect(() => { return () => { stop(); }; }, []);\n"
                  "  // Note: bind::sync_cleanup\n  const f = () => go();\n");
    const auto collision = parse_module(effect_first);
    check_eq(codes(collision), "TESSERA_REACT_BIND_ID;",
             "an effect with a cleanup owns `_cleanup`");
    check_eq(first_diag(collision).message, "duplicate bind id `sync_cleanup`", "message");

    const std::string handler_first =
        component("  // Note: bind::sync_cleanup\n  const f = () => go();\n"
                  "  // Note: bind::sync\n  useEffect(() => { return () => { stop(); }; }, []);\n");
    const auto reverse = parse_module(handler_first);
    check_eq(codes(reverse), "TESSERA_REACT_BIND_ID;", "the reservation also fails in this order");
    check(first_diag(reverse).message.find("reserves `sync_cleanup`") != std::string::npos,
          "message names the reserved id");

    const auto no_cleanup =
        parse_module(component("  // Note: bind::sync\n  useEffect(() => { go(); }, []);\n"
                               "  // Note: bind::sync_cleanup\n  const f = () => go();\n"));
    check(no_cleanup.ok(), "without a cleanup nothing is reserved");

    const auto value_return =
        parse_module(component("  // Note: bind::sync\n  useEffect(() => { return value; }, []);\n"
                               "  // Note: bind::sync_cleanup\n  const f = () => go();\n"));
    check(value_return.ok(), "returning a non-function is not a cleanup: " + codes(value_return));
}

void test_positions() {
    struct Case {
        std::string body;
        const char* what;
    };
    const Case cases[] = {
        {"  // Note: bind::x\n  const plain = 1;\n", "before a non-handler constant"},
        {"  // Note: bind::x\n  const [a, b] = useState<int>(0);\n", "before a hook declaration"},
        {"  // Note: bind::x\n  go();\n", "before a call that is not useEffect"},
        {"  // Note: bind::x\n\n  const f = () => go();\n", "blank line between"},
        {"  // Note: bind::x\n  // second line\n  const f = () => go();\n",
         "not the previous line"},
        {"  const f = () => go(); // Note: bind::x\n", "after the statement on the same line"},
        {"  const f = () => go();\n  // Note: bind::x\n", "before the return"},
    };
    for (const auto& test : cases) {
        const auto result = parse_module(component(test.body));
        check_eq(codes(result), "TESSERA_REACT_BIND_POSITION;", test.what);
        check(!result.root->items[0].valid, std::string("component invalid: ") + test.what);
    }
    check_eq(codes(parse_module("// Note: bind::x\nfunction A() { return <B/>; }\n")),
             "TESSERA_REACT_BIND_POSITION;", "before a component");
    check_eq(codes(parse_module("function A() { return <B/>; }\n// Note: bind::x\n")),
             "TESSERA_REACT_BIND_POSITION;", "after the last item");
    check_eq(codes(parse_module("const k = 1;\n// Note: bind::x\nconst f = () => 1;\n")),
             "TESSERA_REACT_BIND_POSITION;", "top-level handler-looking constant");
    check_eq(codes(parse_module(
                 "function A() {\n  return <B>\n    // Note: bind::x\n    text\n  </B>;\n}\n")),
             "", "inside JSX text a line comment is just text");
    check_eq(codes(parse_module(
                 "function A() {\n  return <B\n    // Note: bind::x\n    a=\"1\"/>;\n}\n")),
             "TESSERA_REACT_BIND_POSITION;", "between attributes");
}

void test_broken_items_add_no_bind_diagnostics() {
    const auto result = parse_module(
        "function A() {\n  // Note: bind::Bad id\n  const = ;\n  return <B/>;\n}\n"
        "function C() {\n  // Note: bind::ok\n  const f = () => go();\n  return <B/>;\n}\n");
    check_eq(codes(result), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "the syntax error is the only diagnostic");
    check_eq(validity_text(result), "IV", "the second component is paired normally");
    check_eq(result.root->items[1].body[0].bind_id, "ok", "pairing continues after recovery");

    // A bind id inside an invalid item is not reserved, so it can be reused later.
    const auto reuse = parse_module(
        "function A() {\n  // Note: bind::x\n  const f = () => go();\n  const = ;\n}\n"
        "function C() {\n  // Note: bind::x\n  const f = () => go();\n  return <B/>;\n}\n");
    check_eq(codes(reuse), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "no duplicate-id error");
}

void test_comments_skipped_by_recovery_are_not_checked() {
    const auto result =
        parse_module("function A() { const = ;\n  // Note: bind::x\n  const y = 1;\n}\n"
                     "function B() { return <B/>; }\n");
    check_eq(codes(result), "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "a comment inside the skipped region of an invalid item is not reported");
}

} // namespace

int main() {
    test_handlers_and_effects_are_paired();
    test_accepted_comment_spellings();
    test_other_comments_are_ignored();
    test_id_rules();
    test_duplicates_and_cleanup_reservation();
    test_positions();
    test_broken_items_add_no_bind_diagnostics();
    test_comments_skipped_by_recovery_are_not_checked();
    return finish("jsx bind tests");
}
