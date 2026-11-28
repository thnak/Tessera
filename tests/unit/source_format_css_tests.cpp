// SPDX-License-Identifier: MIT

// ADR-061 slice 3: CSS and tessera.json. The four properties of slice 1 (idempotent,
// meaning-preserving, deterministic, no comment lost) over hand-written cases and over every .css
// file and tessera.json the repository ships. Meaning for CSS is checked against the stylesheet
// engine's own parser (parse_stylesheet), not only against the tokenizer the printer uses.

#include "source_format_test_support.hpp"
#include "tessera/compiler/format/edits.hpp"
#include "tessera/ui/style/parser.hpp"
#include "tessera/ui/style/tokenizer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace source_format_test {

namespace fs = std::filesystem;
namespace format = tessera::compiler::format;
namespace style = tessera::ui::style;

namespace {

std::string collapse(std::string_view text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !out.empty() && out.back() != ',';
        } else if (c == ',') {
            out += ','; // the printer breaks the line after a selector comma
            space = false;
        } else {
            out += space ? " " : "";
            space = false;
            out += c;
        }
    }
    return out;
}

// The stylesheet engine's reading of a file, with whitespace runs collapsed so that a re-indent
// is not a difference. At-rule blocks are compared by parsing them again.
void describe(std::string_view source, std::string& out) {
    const style::StyleSheet sheet = style::parse_stylesheet(source);
    for (const style::Rule& rule : sheet.rules) {
        out += "R[" + collapse(rule.selector) + "]{";
        for (const style::Declaration& d : rule.declarations) {
            out += collapse(d.property) + ":" + collapse(d.value) + (d.important ? "!" : "") + ";";
        }
        out += "}";
    }
    for (const style::AtRule& at : sheet.at_rules) {
        out += "@" + at.name + "[" + collapse(at.prelude) + "]";
        if (at.has_block) {
            out += "{";
            describe(at.block, out);
            out += "}";
        }
    }
}

std::string comments_of(std::string_view source) {
    std::string out;
    for (const style::Token& t : style::Tokenizer(source).tokenize()) {
        if (t.type == style::TokenType::Comment) {
            for (const char c : t.text) {
                if (c != ' ' && c != '\n' && c != '\t') {
                    out += c;
                }
            }
            out += '|';
        }
    }
    return out;
}

format::FormatResult format_css(std::string_view text, const format::FormatOptions& o = {}) {
    return format::format_source(format::FileKind::Css, text, o);
}

format::FormatResult format_json(std::string_view text, const format::FormatOptions& o = {}) {
    return format::format_source(format::FileKind::Json, text, o);
}

// Returns the formatted text ("" when refused) after checking the four properties.
std::string check_css(std::string_view text, const std::string& what) {
    const auto once = format_css(text);
    check(once.ok(), what + ": formats");
    if (!once.ok()) {
        return {};
    }
    const auto twice = format_css(once.text);
    check(twice.ok() && twice.text == once.text, what + ": idempotent");
    check(format_css(text).text == once.text, what + ": deterministic");
    std::string a;
    std::string b;
    describe(text, a);
    describe(once.text, b);
    check(a == b, what + ": the stylesheet parser reads the same rules\n  " + a + "\n  " + b);
    check(comments_of(text) == comments_of(once.text), what + ": no comment lost");
    check(format::format_edits(format::FileKind::Css, text).ok(), what + ": edit list");
    return once.text;
}

void golden(const char* name, const char* input, const char* expected) {
    const std::string out = check_css(input, name);
    check(out == expected,
          std::string(name) + ": output\n--- got\n" + out + "--- want\n" + expected);
}

void test_css_goldens() {
    golden("declarations", "a{color:red;background : blue!important}b{margin:0 auto}\n",
           "a {\n  color: red;\n  background: blue!important;\n}\nb {\n  margin: 0 auto;\n}\n");
    golden("blank lines", "a{}\n\n\n\nb{c:d}\n.x{\n\n  e:f;\n\n\n  g:h\n}\n",
           "a {}\n\nb {\n  c: d;\n}\n.x {\n  e: f;\n\n  g: h;\n}\n");
    golden("selector list", "a,\n  b > c ,d:is(e, f){x:y}",
           "a,\nb > c,\nd:is(e, f) {\n  x: y;\n}\n");
    golden("at-rule prelude untouched",
           "@media (min-width:600px) and (max-width :900px){a{b:c}}\n@import url(x.css)   screen;\n"
           "@layer  a,b;\n",
           "@media (min-width:600px) and (max-width :900px) {\n  a {\n    b: c;\n  }\n}\n"
           "@import url(x.css)   screen;\n@layer  a,b;\n");
    golden("custom property value untouched", "a{--y:1  2;--z:\n  calc( 1px+2px );}",
           "a {\n  --y: 1  2;\n  --z: calc( 1px+2px );\n}\n");
    // The engine's parser does not read a braced custom property value, so this one is checked
    // against the token stream the printer verifies itself with, and for idempotence.
    const auto braced = format_css("a{--x:  { a:b };c:d}");
    check(braced.ok() && braced.text == "a {\n  --x: { a:b };\n  c: d;\n}\n" &&
              format_css(braced.text).text == braced.text,
          "a braced custom property value is untouched");
    golden(
        "multi-line value keeps its lines",
        "a{grid-template-areas:\n\"a b\"\n   \"c d\";transition: a 1s,\n b 2s}",
        "a {\n  grid-template-areas: \"a b\"\n    \"c d\";\n  transition: a 1s,\n    b 2s;\n}\n");
    golden("strings and urls stay", "a{content:'a  b';background:url( \"x y.png\" )}",
           "a {\n  content: 'a  b';\n  background: url( \"x y.png\" );\n}\n");
    golden("empty and whitespace-only files", "  \n\n", "");
    golden("nesting", "a{b:c;&:hover{d:e}.f &{g:h}}",
           "a {\n  b: c;\n  &:hover {\n    d: e;\n  }\n  .f & {\n    g: h;\n  }\n}\n");
}

void test_css_comments() {
    golden(
        "comments by position",
        "/* head */\na{ /* after brace */\n  /* own line */\n  color:red; /* trailing */\n"
        "  margin:0 /* inside value */ 1px;\n}\n/* between */\nb /* in selector */ {c:d}\n"
        "/* end */",
        "/* head */\na { /* after brace */\n  /* own line */\n  color: red; /* trailing */\n"
        "  margin: 0 /* inside value */ 1px;\n}\n/* between */\nb /* in selector */ {\n  c: d;\n}\n"
        "/* end */\n");
    golden("comment only file", "/* x */", "/* x */\n");
    golden("comment in an empty rule", "a{/* c */}", "a { /* c */\n}\n");
    golden("jsdoc-style block is re-indented", "a{\n      /**\n         * x\n         */\n  b:c}",
           "a {\n  /**\n   * x\n   */\n  b: c;\n}\n");
    golden("other multi-line block is verbatim", "a{\n  /* x\n      y */\n  b:c}",
           "a {\n  /* x\n      y */\n  b: c;\n}\n");
}

void test_css_refusals_and_options() {
    for (const char* bad : {"a{b:c", "a{b:c}}", "a{b:\"x\n}", "a{b:(c}", "a{--x:{}"}) {
        const auto r = format_css(bad);
        check(!r.ok() && r.text.empty() && r.diagnostics[0].code == "TESSERA_FMT_SYNTAX",
              std::string("refused: ") + bad);
    }
    const auto at = format_css("a{b:c}\n}\n");
    check(!at.ok() && at.diagnostics[0].line == 2, "the refusal carries the line");

    format::FormatOptions four;
    four.indent_width = 4;
    check(format_css("a{b:c}", four).text == "a {\n    b: c;\n}\n", "indentWidth applies to CSS");
    check(format_css("\xEF\xBB\xBF"
                     "a{b:c}\r\n")
                  .text == "\xEF\xBB\xBF"
                           "a {\r\n  b: c;\r\n}\r\n",
          "BOM and CRLF follow the input");
    check(!format_css("a {\n  b: c;\n}\n").changed, "a canonical file is unchanged");
    const auto edits = format::format_edits(format::FileKind::Css, "a{b:c}");
    check(edits.ok() && !edits.edits.empty(), "CSS has an edit list");
}

const char* const kJsonMessy =
    "{\"schemaVersion\":1,\"app\":{\"name\":\"x\",\"entry\":\"X.tsx\"},\n\"z\":[1,2,3],"
    "\"a\":[],\"o\":{},\"n\":[{\"k\":null},[true,false]],\"s\":\"\\u00e9\\\"q\",\"f\":-1.5e+3}";

void test_json() {
    const auto r = format_json(kJsonMessy);
    check(r.ok(), "tessera.json formats");
    const std::string want =
        "{\n  \"schemaVersion\": 1,\n  \"app\": {\n    \"name\": \"x\",\n    \"entry\": "
        "\"X.tsx\"\n  },\n"
        "  \"z\": [1, 2, 3],\n  \"a\": [],\n  \"o\": {},\n  \"n\": [\n    {\n      \"k\": null\n   "
        " },\n"
        "    [true, false]\n  ],\n  \"s\": \"\\u00e9\\\"q\",\n  \"f\": -1.5e+3\n}\n";
    check(r.text == want, "json output\n--- got\n" + r.text + "--- want\n" + want);
    check(format_json(r.text).text == r.text && !format_json(r.text).changed, "json is idempotent");

    format::FormatOptions narrow;
    narrow.print_width = 12;
    narrow.indent_width = 4;
    check(format_json("{\"a\":[1,2,3,4,5,6]}", narrow).text ==
              "{\n    \"a\": [\n        1,\n        2,\n        3,\n        4,\n        5,\n       "
              " 6\n    ]\n}\n",
          "a scalar array that does not fit breaks, with indentWidth");
    check(format_json("{\"b\":1,\"a\":2,\"b\":3}").text ==
              "{\n  \"b\": 1,\n  \"a\": 2,\n  \"b\": 3\n}\n",
          "key order and duplicate keys are preserved");
    for (const char* bad : {"", "{", "{\"a\":1,}", "{'a':1}", "[1 2]", "{\"a\":01}", "// c\n{}",
                            "{\"a\":\"\\x\"}", "{} {}", "{\"a\":\"x\ny\"}"}) {
        const auto b = format_json(bad);
        check(!b.ok() && b.diagnostics[0].code == "TESSERA_FMT_SYNTAX",
              std::string("refused json: ") + bad);
    }

    const format::FormatOptions o = format::format_options_from_tessera_json(
        "{\"format\":{\"printWidth\":60,\"indentWidth\":4}}");
    check(o.print_width == 60 && o.indent_width == 4, "format options are read");
    const format::FormatOptions bad_o = format::format_options_from_tessera_json(
        "{\"format\":{\"printWidth\":5,\"indentWidth\":\"x\"}}");
    check(bad_o.print_width == 100 && bad_o.indent_width == 2, "invalid options keep the defaults");
    check(format::format_options_from_tessera_json("not json").print_width == 100,
          "an unparsable tessera.json keeps the defaults");

    check(format::file_kind_for_path("a/b/tessera.json") == format::FileKind::Json &&
              format::file_kind_for_path("C:\\x\\app.module.css") == format::FileKind::Css &&
              format::file_kind_for_path("A.tsx") == format::FileKind::Tsx &&
              !format::file_kind_for_path("package.json").has_value(),
          "file kinds by path");
}

std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void test_corpus() {
    const fs::path root = TESSERA_SOURCE_DIR;
    std::vector<fs::path> css;
    std::vector<fs::path> json;
    for (const char* directory : {"samples", "packages", "tests", "tools/reference",
                                  "tools/wiki/src", "designer/web/src"}) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(root / directory, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->is_directory(ec) && it->path().filename() == "node_modules") {
                it.disable_recursion_pending();
            } else if (it->is_regular_file(ec) && it->path().extension() == ".css") {
                css.push_back(it->path());
            } else if (it->is_regular_file(ec) && it->path().filename() == "tessera.json") {
                json.push_back(it->path());
            }
        }
    }
    std::sort(css.begin(), css.end());
    std::sort(json.begin(), json.end());
    check(css.size() >= 35 && json.size() >= 3, "the CSS corpus is found (" +
                                                    std::to_string(css.size()) + " css, " +
                                                    std::to_string(json.size()) + " json)");
    std::size_t formatted = 0;
    for (const fs::path& path : css) {
        const std::string name = fs::relative(path, root).generic_string();
        const std::string text = slurp(path);
        if (format_css(text).ok()) {
            ++formatted;
            check_css(text, "corpus " + name);
        } else {
            std::cout << "source_format css corpus: refused " << name << ": "
                      << format_css(text).diagnostics[0].message << "\n";
        }
    }
    check(formatted == css.size(), "every shipped .css formats (" + std::to_string(formatted) +
                                       "/" + std::to_string(css.size()) + ")");
    for (const fs::path& path : json) {
        const std::string name = fs::relative(path, root).generic_string();
        const std::string text = slurp(path);
        const auto once = format_json(text);
        check(once.ok(), "corpus " + name + ": formats");
        check(once.ok() && format_json(once.text).text == once.text,
              "corpus " + name + ": idempotent");
    }
    std::cout << "source_format css/json corpus: " << formatted << " css, " << json.size()
              << " json\n";
}

} // namespace

void run_css_json_tests() {
    test_css_goldens();
    test_css_comments();
    test_css_refusals_and_options();
    test_json();
    test_corpus();
}

} // namespace source_format_test
