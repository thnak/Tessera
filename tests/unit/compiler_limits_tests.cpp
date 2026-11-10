// SPDX-License-Identifier: MIT

// Deterministic stress corpus for the front-end resource limits (docs/specs/jsx-subset-v1.md
// sections 14 and 15, docs/design/security.md section 3). Every input is fixed at test time: no
// RNG, clock, filesystem or locale, so a failure reproduces exactly. Each case must terminate
// with a tree or a diagnostic, keep the report bounded, and never build diagnostic text that
// grows with the file. The corpus is the regression net for the limits:
//
//   deep nesting      - every recursive production stops at TESSERA_JSX_LIMIT_DEPTH
//   10k siblings      - wide (not deep) input is accepted, so the limit is not a size limit
//   ~1 MB file        - the file, token and diagnostics caps fire before parsing can blow up
//   pathological text - unbounded chains, floods and long identifiers stay bounded
//   invalid UTF-8     - every bad sequence is reported at its offset and parsing continues

#include "jsx_test_util.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

enum class Entry { kJsx, kExpression, kType, kModule };

struct Outcome {
    std::string codes;
    std::string text; // formatted diagnostics, for the determinism comparison
    std::size_t diagnostics = 0;
    bool tree = false;
    bool truncated = false;
};

std::string repeat(std::string_view text, std::size_t count) {
    std::string out;
    out.reserve(text.size() * count);
    for (std::size_t i = 0; i < count; ++i) {
        out.append(text);
    }
    return out;
}

// The invariant of every parse: a tree or at least one error, a report bounded by the limit, and
// no diagnostic whose text grows with the input.
template<typename Result>
Outcome measure(const Result& result, const Limits& limits, std::string_view what) {
    Outcome out;
    out.tree = result.root != nullptr;
    out.truncated = result.diagnostics_truncated;
    out.diagnostics = result.diagnostics.size();
    for (const auto& diagnostic : result.diagnostics) {
        out.codes += std::string(diagnostic.code) + ";";
        out.text += format_diagnostic(diagnostic) + "\n";
        check(!diagnostic.code.empty(), "a code: " + std::string(what));
        check(diagnostic.message.size() <= kMaxDiagnosticTextBytes + 3,
              "bounded message: " + std::string(what));
        check(diagnostic.suggestion.size() <= kMaxDiagnosticTextBytes + 3,
              "bounded suggestion: " + std::string(what));
        if (diagnostic.related) {
            check(diagnostic.related->message.size() <= kMaxDiagnosticTextBytes + 3,
                  "bounded related text: " + std::string(what));
        }
    }
    check(out.tree || out.diagnostics > 0, "tree or diagnostic: " + std::string(what));
    check(out.diagnostics <= limits.max_diagnostics + 1, "bounded report: " + std::string(what));
    return out;
}

Outcome parse_case(Entry entry, std::string_view source, const Limits& limits,
                   std::string_view what) {
    switch (entry) {
    case Entry::kJsx:
        return measure(parse_jsx_element(source, limits), limits, what);
    case Entry::kExpression:
        return measure(parse_expression(source, limits), limits, what);
    case Entry::kType:
        return measure(parse_type(source, limits), limits, what);
    case Entry::kModule:
        return measure(parse_module(source, limits), limits, what);
    }
    return {};
}

void check_deterministic(Entry entry, const std::string& source, std::string_view what) {
    const Outcome first = parse_case(entry, source, Limits{}, what);
    const Outcome second = parse_case(entry, source, Limits{}, what);
    check_eq(second.codes, first.codes, "same codes twice: " + std::string(what));
    check_eq(second.text, first.text, "same report twice: " + std::string(what));
}

struct Case {
    const char* name;
    Entry entry;
    std::string source;
};

// A recursive production deeper than the limit is a diagnostic, not a stack overflow. The AST
// height of a long operator chain counts too, so deleting the tree stays bounded.
void test_deep_nesting(const std::vector<Case>& cases) {
    for (const auto& test : cases) {
        const Outcome out = parse_case(test.entry, test.source, Limits{}, test.name);
        check_eq(out.codes, "TESSERA_JSX_LIMIT_DEPTH;", std::string("depth: ") + test.name);
        // A module always comes back with the broken item marked invalid; the other entries only
        // produce a tree when the structure itself is clear.
        const bool module = test.entry == Entry::kModule;
        check(out.tree == module, std::string("tree presence: ") + test.name);
        check_deterministic(test.entry, test.source, test.name);
    }
}

void test_wide_inputs_are_accepted() {
    const auto siblings = parse_jsx_element("<A>" + repeat("<B/>", 10000) + "</A>");
    check(siblings.ok(), "10k sibling elements parse");
    check(siblings.root && siblings.root->children.size() == 10000, "10k children are kept");

    std::string attributes = "<A";
    for (std::size_t i = 0; i < 10000; ++i) {
        attributes += " p" + std::to_string(i) + "=\"v\"";
    }
    attributes += "/>";
    const auto many_attributes = parse_jsx_element(attributes);
    check(many_attributes.ok(), "10k attributes parse");
    check(many_attributes.root && many_attributes.root->attributes.size() == 10000,
          "10k attributes are kept");

    check(parse_jsx_element("<A>" + repeat("x", 4096) + "</A>").ok(), "4 KiB text node");
    check(parse_expression("[" + repeat("1,", 9999) + "1]").ok(), "10k array elements");
    check(parse_expression("f(" + repeat("1,", 9999) + "1)").ok(), "10k call arguments");
    check(parse_expression("{" + repeat("a:1,", 9999) + "a:1}").ok(), "10k object properties");
    check(parse_expression("(" + repeat("a,", 9999) + "a)=>a").ok(), "10k arrow parameters");
    check(parse_type("A" + repeat("|A", 9999)).ok(), "10k union members");
    check(parse_type("{" + repeat("a:int;", 9999) + "a:int}").ok(), "10k object type members");
    check(parse_module(repeat("const a = 1;\n", 10000)).ok(), "10k module items");
    check(parse_module("import { " + repeat("a,", 9999) + "a } from \"tessera\";").ok(),
          "10k import names");
    check(parse_module("function A() { " + repeat("const a = 1;\n", 10000) + "return <A/>; }").ok(),
          "10k statements in a component");
}

// One file exactly at the size limit is accepted; one byte more is not.
void test_file_size_boundary() {
    const Limits limits;
    const std::string body(limits.max_file_bytes - 7, 'x');
    const std::string exact = "<A>" + body + "</A>";
    check(exact.size() == limits.max_file_bytes, "the fixture is exactly at the limit");
    const auto accepted = parse_jsx_element(exact);
    check(accepted.ok() && accepted.root->children.size() == 1 &&
              accepted.root->children[0]->text.size() == body.size(),
          "a 1 MiB file exactly at the limit parses");
    check_eq(codes(parse_jsx_element(exact + " ")), "TESSERA_JSX_LIMIT_FILE_SIZE;",
             "one byte over the limit");
}

// Each huge input stops at a specific limit instead of running away. `twice` is false only for
// the cases whose second parse would double a multi-megabyte scan for no extra coverage.
struct Huge {
    const char* name;
    Entry entry;
    std::string source;
    const char* first_code;
    bool tree;
    bool twice = true;
};

void test_huge_inputs(const std::vector<Huge>& cases) {
    for (const auto& test : cases) {
        const Outcome out = parse_case(test.entry, test.source, Limits{}, test.name);
        const std::string first =
            out.codes.empty() ? "" : out.codes.substr(0, std::string(test.first_code).size() + 1);
        check_eq(first, std::string(test.first_code) + ";",
                 std::string("first code: ") + test.name);
        check(out.tree == test.tree, std::string("tree presence: ") + test.name);
        if (test.twice) {
            check_deterministic(test.entry, test.source, test.name);
        }
    }
}

void test_limit_boundaries() {
    Limits small;
    small.max_file_bytes = 16;
    check(parse_jsx_element("<A>x</A>", small).ok(), "a file under the small size limit");
    check_eq(codes(parse_jsx_element(std::string(17, 'x'), small)), "TESSERA_JSX_LIMIT_FILE_SIZE;",
             "a file over the small size limit");

    Limits few_tokens;
    few_tokens.max_tokens = 3;
    check(parse_expression("a+b", few_tokens).ok(), "exactly 3 tokens fit a limit of 3");
    check_eq(codes(parse_expression("a+b+c", few_tokens)), "TESSERA_JSX_LIMIT_TOKENS;",
             "5 tokens do not fit a limit of 3");

    Limits shallow;
    shallow.max_nesting_depth = 4;
    check(parse_jsx_element(repeat("<A>", 4) + repeat("</A>", 4), shallow).ok(),
          "4 nested elements fit a limit of 4");
    check_eq(codes(parse_jsx_element(repeat("<A>", 5) + repeat("</A>", 5), shallow)),
             "TESSERA_JSX_LIMIT_DEPTH;", "5 nested elements do not");

    Limits few_diagnostics;
    few_diagnostics.max_diagnostics = 3;
    const auto flood = parse_jsx_element("<A>" + repeat("&bogus;", 10) + "</A>", few_diagnostics);
    check(flood.diagnostics.size() == 4, "3 diagnostics plus the truncation marker");
    check(flood.diagnostics_truncated, "the report is flagged as truncated");
    check_eq(std::string(flood.diagnostics.back().code), "TESSERA_JSX_LIMIT_DIAGNOSTICS",
             "the marker ends the report");
    check(flood.root != nullptr, "the tree is still built");

    Limits few_components;
    few_components.max_components = 2;
    const auto components =
        parse_module(repeat("function A() { return <A/>; }\n", 3), few_components);
    check_eq(codes(components), "TESSERA_JSX_LIMIT_COMPONENTS;", "3 components with a limit of 2");
    check(components.root && components.root->items.size() == 2, "the first 2 items are kept");

    // Every limit is reported once, not once per following token.
    Limits small_tokens;
    small_tokens.max_tokens = 32;
    check_eq(codes(parse_module(repeat("const a=1;", 100), small_tokens)),
             "TESSERA_JSX_LIMIT_TOKENS;", "one token-limit diagnostic per parse");
}

void test_invalid_utf8() {
    // A bad byte inside an attribute string is reported once, at its offset, and the tree is
    // still built: one malformed sequence does not hide the rest of the file.
    const auto inside_string = parse_jsx_element(std::string("<A b=\"\xFF\"/>"));
    check_eq(codes(inside_string), "TESSERA_JSX_LEX_INVALID_UTF8;", "a bad byte in a string");
    check(inside_string.root != nullptr, "the tree survives the bad byte");
    check(inside_string.diagnostics.size() == 1 &&
              inside_string.diagnostics[0].span.span.offset == 6,
          "the bad byte is reported at its offset");

    // A sequence cut off by the end of the file and an overlong encoding are both invalid.
    const auto truncated = parse_jsx_element(std::string("<A/>\xE2\x82"));
    check(std::string(first_diag(truncated).code) == "TESSERA_JSX_LEX_INVALID_UTF8" &&
              truncated.diagnostics[0].span.span.offset == 4,
          "a truncated 3-byte sequence at the end of the file");
    const auto overlong = parse_jsx_element(std::string("<A b=\"\xC0\xAF\"/>"));
    check(std::string(first_diag(overlong).code) == "TESSERA_JSX_LEX_INVALID_UTF8" &&
              overlong.diagnostics[0].span.span.offset == 6,
          "an overlong encoding");

    const auto lone_cr = parse_jsx_element(std::string("<A>\r</A>"));
    check_eq(codes(lone_cr), "TESSERA_JSX_LEX_LONE_CR;", "a lone CR is reported");

    // Valid 2-, 3- and 4-byte sequences are ordinary text.
    check(parse_jsx_element("<A>h\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80</A>").ok(),
          "valid multi-byte text parses");

    // A file of bad bytes terminates with the cap, not with a crash or a hang.
    Limits limits;
    limits.max_diagnostics = 16;
    const auto flood = parse_jsx_element(repeat("\xFF", 4096), limits);
    check(flood.diagnostics.size() == limits.max_diagnostics + 1, "the UTF-8 flood is capped");
    check(flood.diagnostics_truncated, "the flood is flagged as truncated");
}

} // namespace

int main() {
    constexpr std::size_t kDeep = 3000;
    const std::size_t kFlood = 512 * 1024;
    test_deep_nesting({
        {"parentheses", Entry::kExpression, repeat("(", kDeep) + "1" + repeat(")", kDeep)},
        {"unary", Entry::kExpression, repeat("!", kDeep) + "a"},
        {"arrays", Entry::kExpression, repeat("[", kDeep) + repeat("]", kDeep)},
        {"objects", Entry::kExpression, repeat("{a:", kDeep) + "1" + repeat("}", kDeep)},
        {"templates", Entry::kExpression, repeat("`${", kDeep)},
        {"calls", Entry::kExpression, repeat("f(", kDeep) + repeat(")", kDeep)},
        {"arrows", Entry::kExpression, repeat("x=>", kDeep) + "x"},
        {"conditionals", Entry::kExpression, repeat("a?b:", kDeep) + "c"},
        {"member chain", Entry::kExpression, "a" + repeat(".b", kDeep)},
        {"binary chain", Entry::kExpression, "a" + repeat("+a", kDeep)},
        {"elements", Entry::kJsx, repeat("<A>", kDeep)},
        {"fragments", Entry::kJsx, repeat("<>", kDeep)},
        {"attribute expressions", Entry::kJsx, repeat("<A b={", kDeep)},
        {"generic types", Entry::kType, repeat("A<", kDeep)},
        {"array suffix chain", Entry::kType, "A" + repeat("[]", kDeep)},
        {"object types", Entry::kType, repeat("{a:", kDeep) + "1" + repeat("}", kDeep)},
        {"function types", Entry::kType, repeat("(a: ", kDeep) + "int" + repeat(") => int", kDeep)},
        {"module type alias", Entry::kModule, "type T = " + repeat("A<", kDeep)},
        {"module constant", Entry::kModule, "const a = " + repeat("(", kDeep)},
        {"component elements", Entry::kModule, "function A() { return " + repeat("<A>", kDeep)},
        {"component calls", Entry::kModule, "function A() { const [a] = " + repeat("f(", kDeep)},
    });
    test_wide_inputs_are_accepted();
    test_file_size_boundary();
    test_huge_inputs({
        {"file over the limit", Entry::kJsx, "<A>" + repeat("x", 1024 * 1024),
         "TESSERA_JSX_LIMIT_FILE_SIZE", false},
        {"invalid UTF-8 flood", Entry::kJsx, repeat("\xFF", kFlood), "TESSERA_JSX_LEX_INVALID_UTF8",
         false, false},
        {"entity flood", Entry::kJsx, "<A>" + repeat("&bogus;", 64 * 1024) + "</A>",
         "TESSERA_JSX_PARSE_UNKNOWN_ENTITY", true, false},
        {"escape flood", Entry::kExpression, "\"\\u{" + repeat("a", kFlood),
         "TESSERA_JSX_LEX_INVALID_ESCAPE", true},
        {"number literal flood", Entry::kExpression, "1" + repeat("a", kFlood),
         "TESSERA_JSX_LEX_INVALID_NUMBER", true},
        {"lowercase tag flood", Entry::kJsx, "<" + repeat("a", kFlood) + "/>",
         "TESSERA_JSX_SUBSET_HTML_TAG", true},
        {"unclosed tag flood", Entry::kJsx, "<" + repeat("A", kFlood),
         "TESSERA_JSX_PARSE_UNCLOSED_ELEMENT", false},
        {"member tag flood", Entry::kJsx, "<" + repeat("A.", 40000) + "A/>",
         "TESSERA_JSX_SUBSET_DYNAMIC_TAG", true, false},
        {"parentheses at the file limit", Entry::kExpression, repeat("(", kFlood),
         "TESSERA_JSX_LIMIT_DEPTH", false},
        {"attributes at the file limit", Entry::kJsx, repeat("<A b={", kFlood / 7),
         "TESSERA_JSX_LIMIT_DEPTH", false},
        {"unterminated string flood", Entry::kExpression, "\"" + repeat("x", kFlood),
         "TESSERA_JSX_LEX_UNTERMINATED_STRING", true},
        {"token flood in a module", Entry::kModule, repeat("const a=1;", 40001),
         "TESSERA_JSX_LIMIT_TOKENS", true, false},
        {"object flood in a module", Entry::kModule, "const a = " + repeat("{a:", kFlood / 3) + "1",
         "TESSERA_JSX_LIMIT_DEPTH", true, false},
    });
    test_limit_boundaries();
    test_invalid_utf8();
    return finish("compiler limits tests");
}
