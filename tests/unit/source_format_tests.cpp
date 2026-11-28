// SPDX-License-Identifier: MIT

// Golden, property and refusal tests for the compiler-owned formatter (ADR-061 slice 1,
// docs/specs/source-format-v1.md). Every input is fixed text: no clock, locale or filesystem
// (the corpus and CLI tests live in their own files and read the repository).

#include "source_format_test_support.hpp"
#include "tessera/compiler/jsx/parser.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace source_format_test {

using tessera::compiler::format::FormatOptions;

namespace {

std::string comment_texts(const tessera::compiler::jsx::ModuleParseResult& parsed) {
    std::string out;
    for (const auto& comment : parsed.comments) {
        for (const char c : comment.text) {
            if (c != ' ' && c != '\n' && c != '\t') {
                out.push_back(c);
            }
        }
        out.push_back('|');
    }
    return out;
}

void test_goldens() {
    struct Case {
        const char* name;
        const char* input;
        const char* expected;
    };
    const std::vector<Case> cases = {
        {"imports, types, hooks, handlers, JSX",
         R"(import {useState,useEffect} from 'tessera';
type Props = { title: string, count?: int; items: string[] };
type Mode = "a"|"b"|   "c";
export function Card({title,count=0,items}:Props){
  const [n,setN]=useState<int>(0); // trailing n
  const label=n>0?"many":'none';
  const onTap=()=>{
    if(n>3){setN(0);}else{setN(n+1);}
    // dangling end
  };
  useEffect(()=>{ setN(1); },[count]);
  return <View className={`a ${label}`} onClick={onTap}>
    {/* a jsx comment */}
    {n>0 && <Text>{title} is {label}</Text>}
    {items.map((it)=><Text key={it}>{it}</Text>)}
  </View>;
}
)",
         R"(import { useState, useEffect } from "tessera";
type Props = { title: string; count?: int; items: string[] };
type Mode = "a" | "b" | "c";
export function Card({ title, count = 0, items }: Props) {
  const [n, setN] = useState<int>(0); // trailing n
  const label = n > 0 ? "many" : "none";
  const onTap = () => {
    if (n > 3) {
      setN(0);
    } else {
      setN(n + 1);
    }
    // dangling end
  };
  useEffect(() => {
    setN(1);
  }, [count]);
  return (
    <View className={`a ${label}`} onClick={onTap}>
      {/* a jsx comment */}
      {n > 0 && <Text>{title} is {label}</Text>}
      {items.map((it) => <Text key={it}>{it}</Text>)}
    </View>
  );
}
)"},
        {"comments around a call and an object",
         "export function A() {\n  const x = foo( // after paren\n    1, // one\n    2 // two\n  "
         ");\n"
         "  const o = {\n    a: 1, // one\n\n    // lead b\n    b: 2,\n    // end\n  };\n  return "
         "<View />;\n}\n",
         "export function A() {\n  const x = foo( // after paren\n    1, // one\n    2 // two\n  "
         ");\n"
         "  const o = {\n    a: 1, // one\n\n    // lead b\n    b: 2,\n    // end\n  };\n  return "
         "<View />;\n}\n"},
        {"comments inside a tag",
         "export function A() {\n  return <View /* c */ a=\"1\" // line\n    b=\"2\" />;\n}\n",
         "export function A() {\n  return (\n    <View /* c */\n      a=\"1\" // line\n      "
         "b=\"2\"\n    />\n  );\n}\n"},
        {"a block comment between operands stays where it is",
         "export function A() {\n  const y = a + /* mid */ b;\n  return <View />;\n}\n",
         "export function A() {\n  const y = a + /* mid */ b;\n  return <View />;\n}\n"},
        {"prose wraps at the width and keeps inline elements",
         "export function A() {\n  return <View>hello <b>x</b> world, a very long sentence that "
         "keeps going and going so that it must "
         "wrap across lines yes <i>ital</i> end</View>;\n}\n",
         "export function A() {\n  return (\n    <View>\n      hello <b>x</b> world, a very long "
         "sentence that keeps going and going so that it must "
         "wrap\n      across lines yes <i>ital</i> end\n    </View>\n  );\n}\n"},
        {"a file of one comment", "// only a comment\n", "// only a comment\n"},
        {"an empty file", "", ""},
        {"a long arrow body moves under the arrow",
         "export function A() {\n  const f = (event: KeyEvent) => (event.key === \"Enter\" || "
         "event.key === \" \") && !disabled && "
         "setPressed(true);\n  return <View />;\n}\n",
         "export function A() {\n  const f = (event: KeyEvent) =>\n    (event.key === \"Enter\" || "
         "event.key === \" \") && !disabled && "
         "setPressed(true);\n  return <View />;\n}\n"},
    };
    for (const Case& c : cases) {
        const auto result = format_tsx(c.input);
        check(result.ok(), std::string("golden formats: ") + c.name);
        check(result.text == c.expected, std::string("golden text: ") + c.name + "\n--- got\n" +
                                             result.text + "--- want\n" + c.expected);
        check_properties(c.input, std::string("golden: ") + c.name);
    }
}

void test_properties_on_awkward_input() {
    const std::vector<std::pair<const char*, const char*>> inputs = {
        {"lone-line component", "export function A(){return <View/>;}"},
        {"switch with labels",
         "type S = { n: int };\nfunction r(state: S, action: A): S {\n  switch (action.type) {\n"
         "    case \"inc\":\n      return state;\n    default:\n      return state;\n  }\n}\n"
         "export function A() { return <View/>; }\n"},
        {"fragment and nested conditionals",
         "export function A() { const a = true; return <>{a ? <A/> : <B/>}{a && <C/>}</>; }\n"},
        {"significant spaces between inline elements",
         "export function A() { return <View><b>a</b> <i>b</i>{\" \"}tail</View>; }\n"},
        {"jsdoc block", "/**\n * Docs\n   * more\n */\nexport function A() { return <View/>; }\n"},
        {"blank lines between statements and children",
         "export function A() {\n  const a = 1;\n\n\n  const b = 2;\n  return <View>\n    <A/>\n\n "
         "   <B/>\n  </View>;\n}\n"},
    };
    for (const auto& [name, input] : inputs) {
        check_properties(input, std::string("properties: ") + name);
    }
}

void test_refusals() {
    // Unrecoverable syntax: not formatted, and the diagnostics say why.
    const auto syntax = format_tsx("export function A( { return <View/>; }\n");
    check(!syntax.ok() && syntax.text.empty(), "a syntax error is a refusal with no output");
    check(!syntax.diagnostics.empty() && syntax.diagnostics[0].code == "TESSERA_FMT_SYNTAX",
          "the refusal is TESSERA_FMT_SYNTAX");

    // A comment that no rule can place: refused, and the diagnostic names it.
    const auto comment = format_tsx("export function A() {\n  const z = a\n    // own line "
                                    "mid-expression\n    + b;\n  return <View />;\n}\n");
    check(!comment.ok() && comment.text.empty(), "an unplaceable comment refuses the file");
    check(!comment.diagnostics.empty() && comment.diagnostics[0].code == "TESSERA_FMT_COMMENT" &&
              comment.diagnostics[0].message.find("own line mid-expression") != std::string::npos &&
              comment.diagnostics[0].line == 3,
          "the refusal names the comment and its line");

    // A comment inside a template substitution cannot be reprinted either.
    const auto templ =
        format_tsx("export function A() {\n  const s = `a ${/* c */ b}`;\n  return <View />;\n}\n");
    check(!templ.ok(), "a comment inside a template literal is refused, not dropped");
}

void test_options_and_line_endings() {
    FormatOptions narrow;
    narrow.print_width = 30;
    const auto result = tessera::compiler::format::format_source(
        tessera::compiler::format::FileKind::Tsx,
        "export function A() { const f = foo(argumentNumberOne, argumentNumberTwo); return <View "
        "/>; }\n",
        narrow);
    check(result.ok() && result.text.find("argumentNumberOne,\n") != std::string::npos,
          "print_width breaks a call that does not fit");

    FormatOptions wide;
    wide.indent_width = 4;
    const auto indented = tessera::compiler::format::format_source(
        tessera::compiler::format::FileKind::Tsx, "export function A() { return <View />; }\n",
        wide);
    check(indented.ok() && indented.text.find("\n    return") != std::string::npos,
          "indent_width is honoured");

    const auto crlf = format_tsx("export function A(){\r\nreturn <View/>;\r\n}\r\n");
    check(crlf.ok() && crlf.text == "export function A() {\r\n  return <View />;\r\n}\r\n",
          "CRLF input keeps CRLF");
    const auto bom = format_tsx("\xEF\xBB\xBF"
                                "export function A(){return <View/>;}\n");
    check(bom.ok() && bom.text.starts_with("\xEF\xBB\xBF"), "a byte order mark is kept");

    const auto first = format_tsx("export function A(){return <View a='x'/>;}\n");
    const auto second = format_tsx("export function A(){return <View a='x'/>;}\n");
    check(first.text == second.text, "formatting is deterministic");
}

} // namespace

std::string check_properties(std::string_view text, const std::string& what) {
    namespace jsx = tessera::compiler::jsx;
    const auto once = format_tsx(text);
    if (!once.ok()) {
        check(false, what + ": refused: " +
                         (once.diagnostics.empty()
                              ? std::string("?")
                              : once.diagnostics[0].code + " " + once.diagnostics[0].message));
        return {};
    }
    const auto twice = format_tsx(once.text);
    check(twice.ok() && twice.text == once.text, what + ": idempotent");
    const auto before = jsx::parse_module(text);
    const auto after = jsx::parse_module(once.text);
    check(before.root != nullptr && after.root != nullptr &&
              jsx::dump(*before.root) == jsx::dump(*after.root),
          what + ": same AST ignoring trivia");
    check(comment_texts(before) == comment_texts(after), what + ": no comment lost");
    return once.text;
}

} // namespace source_format_test

int main() {
    using namespace source_format_test;
    test_goldens();
    test_properties_on_awkward_input();
    test_refusals();
    test_options_and_line_endings();
    run_corpus_tests();
    run_edits_tests();
    run_css_json_tests();
    run_cli_tests();
    return failures() == 0 ? 0 : 1;
}
