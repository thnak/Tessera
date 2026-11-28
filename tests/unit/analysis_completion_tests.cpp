// SPDX-License-Identifier: MIT

// Completions (issue #573, ADR-060): a list per context (JSX tag, attribute, CSS class, import
// path, bind:: member, expression), deterministic order, UTF-16 ranges and the worker ABI.

#include "analysis_entry.h"
#include "jsx_test_util.hpp"
#include "tessera/compiler/analysis/completion.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace tessera::compiler::analysis;
using jsx_test::check;
using jsx_test::check_eq;

namespace {

const std::string kCard =
    "export function Card({ title, count }: { title: string; count: int }) {\n"
    "  return <p>{title}</p>;\n"
    "}\n";
const std::string kCss = ".card { color: red; }\n"
                         "/* .ghost { } */\n"
                         "a.title:hover { background: url(a.png); margin: 1.5em; }\n"
                         "@media (min-width: 1.5em) { .wide { margin: .5em; } }\n";

// `|` marks the caret; the rest is the text of src/App.tsx.
struct Case {
    ProjectIndex index;
    std::uint32_t offset = 0;
    CompletionList list;
};

Case ask(std::string marked, std::vector<SourceFile> others = {}) {
    const auto bar = marked.find('|');
    const std::uint32_t unit = static_cast<std::uint32_t>(bar);
    marked.erase(bar, 1);
    std::vector<SourceFile> files{
        {0, "src/App.tsx", marked}, {1, "src/Card.tsx", kCard}, {2, "src/App.module.css", kCss}};
    for (SourceFile& f : others) {
        files.push_back(std::move(f));
    }
    Case out;
    out.index = ProjectIndex::build(std::move(files));
    out.offset = unit;
    out.list = complete(out.index, 0, unit);
    return out;
}

std::string labels(const CompletionList& list) {
    std::string out;
    for (const CompletionItem& item : list.items) {
        out += (out.empty() ? "" : ",") + item.label;
    }
    return out;
}

const std::string kHead =
    "import { Card } from \"./Card\";\nimport styles from \"./App.module.css\";\n";

void test_tag() {
    const Case c = ask(kHead + "function Local() { return <i />; }\n"
                               "export function App() {\n  return <Ca|\n}\n");
    check_eq(c.list.context, "tag", "tag context");
    check_eq(labels(c.list), "Card", "a prefix keeps the matching component");
    check_eq(c.list.items[0].kind, "import", "the import is offered with its source");
    const Case all = ask(kHead + "function Local() { return <i />; }\n"
                                 "export function App() {\n  return <|\n}\n");
    check_eq(labels(all.list), "App,Card,Local", "every component in scope, sorted");
    check(all.list.from == all.list.to, "an empty prefix replaces nothing");
}

void test_attribute() {
    const Case c = ask(kHead + "export function App() {\n  return <Card ti| />;\n}\n");
    check_eq(c.list.context, "attribute", "attribute context");
    check_eq(labels(c.list), "title", "a prop of the component, through the import");
    check_eq(c.list.items[0].kind, "prop", "kind prop");
    const Case rest = ask(kHead + "export function App() {\n  return <Card count={1} |/>;\n}\n");
    check_eq(labels(rest.list), "key,title", "a written attribute is not offered again");
    const Case intrinsic = ask(kHead + "export function App() {\n  return <p |/>;\n}\n");
    check_eq(labels(intrinsic.list), "className,key",
             "an intrinsic tag offers the common attributes");
    const Case value = ask(kHead + "export function App() {\n  return <Card title=|/>;\n}\n");
    check(value.list.items.empty(), "an attribute value is not an attribute name");
    const Case expr =
        ask(kHead + "export function App() {\n  const n = 1;\n  return <Card title={n|} />;\n}\n");
    check_eq(expr.list.context, "expression", "an expression container is an expression");
}

void test_css_class() {
    const Case c =
        ask(kHead + "export function App() {\n  return <p className={styles.|} />;\n}\n");
    check_eq(c.list.context, "cssClass", "css class context");
    check_eq(labels(c.list), "card,title,wide",
             "classes of the style sheet, not comments or values");
    const Case some =
        ask(kHead + "export function App() {\n  return <p className={styles.W|} />;\n}\n");
    check_eq(labels(some.list), "wide", "prefix matching ignores case");
    const Case other =
        ask(kHead + "export function App() {\n  const n = [1];\n  return <p>{n.|}</p>;\n}\n");
    check_eq(other.list.context, "member", "another member access needs a type");
    check(other.list.items.empty(), "no guesses for a member of a value");
}

void test_import_path() {
    const Case c =
        ask("import { X } from \"./|\";\n", {{3, "lib/util.ts", ""}, {4, "src/ui/Button.tsx", ""}});
    check_eq(c.list.context, "importPath", "import path context");
    check_eq(labels(c.list), "./App.module.css,./Card,./ui/Button",
             "relative specifiers without .tsx/.ts");
    check(c.list.from == 19 && c.list.to == 21, "the range is the string content");
    const Case up = ask("import { X } from \"../|\";\n", {{3, "lib/util.ts", ""}});
    check_eq(labels(up.list), "../lib/util", "a file outside the directory goes through ..");
    const Case bare = ask("import \"./Ca|\";\n");
    check_eq(labels(bare.list), "./Card", "a side-effect import completes too");
    const Case plain = ask("const s = \"./|\";\n");
    check_eq(plain.list.context, "none", "an ordinary string completes nothing");
}

void test_bind_member() {
    const Case c = ask("export function App() {\n"
                       "  const handleClick = () => {};\n"
                       "  // Note: bind::on_save\n"
                       "  // Note: bind::|\n"
                       "  return <p />;\n}\n");
    check_eq(c.list.context, "bind", "bind context");
    check_eq(labels(c.list), "handle_click,on_save", "ids in use plus the component's handlers");
    check_eq(c.list.items[0].detail, "handler handleClick", "a handler names its source");
    const Case typed = ask("// bind::on_a\n// Note: bind::on_|\n");
    check_eq(labels(typed.list), "on_a", "the word being typed is not offered");
    const Case plain = ask("// Note: |\n");
    check_eq(plain.list.context, "none", "a comment completes nothing else");
}

void test_expression() {
    const Case c = ask(
        kHead +
        "export function App() {\n  const [n, setN] = useState<int>(0);\n"
        "  const bump = (): void => { setN(n + 1); };\n  const x = n + |1;\n  return <p />;\n}\n");
    check_eq(c.list.context, "expression", "expression context");
    check_eq(labels(c.list), "App,Card,bump,n,setN,styles,x", "scope symbols, sorted");
    const Case ns =
        ask(kHead +
            "export function App() {\n  const [n, setN] = useState<int>(0);\n  const x = s|;\n}\n");
    check_eq(labels(ns.list), "setN,styles", "prefix filter");
    const Case lower =
        ask(kHead +
            "export function App() {\n  const [n, setN] = useState<int>(0);\n  const x = S|;\n}\n");
    check_eq(labels(lower.list), "setN,styles", "case-insensitive prefix");
}

void test_none_in_string_and_comment() {
    check(ask(kHead + "export function App() {\n  const x = \"ab|\";\n}\n").list.items.empty(),
          "string");
    check(ask(kHead + "// App |\n").list.items.empty(), "comment");
    check(complete(ask("x|").index, 2, 0).items.empty(), "a css file completes nothing");
    check(complete(ask("x|").index, 99, 0).items.empty(), "an unknown file completes nothing");
}

void test_deterministic_and_utf16() {
    const std::string app =
        "// \xF0\x9F\x98\x80\n" + kHead + "export function App() {\n  return <Ca />;\n}\n";
    const std::vector<SourceFile> files{{0, "src/App.tsx", app}, {1, "src/Card.tsx", kCard}};
    const ProjectIndex a = ProjectIndex::build(files);
    const ProjectIndex b = ProjectIndex::build({files[1], files[0]});
    // UTF-16: the emoji is two units but four bytes.
    const std::string_view marker = "<Ca";
    std::uint32_t unit = 0;
    for (std::size_t i = 0; i < app.find(marker) + 3; ++i) {
        const auto lead = static_cast<unsigned char>(app[i]);
        unit += lead >= 0xF0 ? 2 : ((lead & 0xC0) == 0x80 ? 0 : 1);
    }
    const std::string ja = completion_to_json(a, 0, unit);
    check_eq(ja, completion_to_json(b, 0, unit), "file order does not matter");
    const CompletionList list = complete(a, 0, unit);
    check(list.to == unit && list.from == unit - 2, "the range is in UTF-16 units");
    check_eq(labels(list), "Card", "complete after an emoji");
    check(ja.starts_with("{\"from\":"), "json shape");
}

void test_abi() {
    const Case c = ask(kHead + "export function App() {\n  return <Ca|\n}\n");
    tessera_analysis_reset();
    tessera_analysis_set_file(0, "src/App.tsx", c.index.text(0)->data());
    tessera_analysis_set_file(1, "src/Card.tsx", kCard.c_str());
    tessera_analysis_commit_snapshot();
    const char* text =
        tessera_analysis_request_json("completion", 0, static_cast<int>(c.offset), 0);
    const std::string out = text == nullptr ? "<null>" : text;
    tessera_free(text);
    const ProjectIndex index = ProjectIndex::build(
        {{0, "src/App.tsx", std::string(*c.index.text(0))}, {1, "src/Card.tsx", kCard}});
    check_eq(out, completion_to_json(index, 0, c.offset), "abi completion");
    check(out.find("\"Card\"") != std::string::npos, "abi finds the component");
}

} // namespace

int main() {
    test_tag();
    test_attribute();
    test_css_class();
    test_import_path();
    test_bind_member();
    test_expression();
    test_none_in_string_and_comment();
    test_deterministic_and_utf16();
    test_abi();
    return jsx_test::failures == 0 ? 0 : 1;
}
