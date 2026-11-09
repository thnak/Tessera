// SPDX-License-Identifier: MIT

// Hostile and degenerate input. The front end must always return either a tree or diagnostics:
// no crash, no hang, no unbounded memory (docs/specs/jsx-subset-v1.md sections 14 and 15).

#include "jsx_test_util.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

using namespace jsx_test;
using namespace tessera::compiler::jsx;

std::string repeat(std::string_view text, std::size_t count) {
    std::string out;
    out.reserve(text.size() * count);
    for (std::size_t i = 0; i < count; ++i) {
        out.append(text);
    }
    return out;
}

// The invariant of every parse: a tree or at least one error, and a bounded report.
template<typename Result>
void check_invariant(const Result& result, const Limits& limits, const std::string& what) {
    check(result.root != nullptr || result.has_errors(), "tree or error: " + what);
    check(result.diagnostics.size() <= limits.max_diagnostics + 1, "bounded report: " + what);
}

void test_deep_nesting_is_a_diagnostic() {
    const std::size_t depth = 3000;
    struct Case {
        std::string name;
        std::string source;
        bool jsx;
    };
    const Case cases[] = {
        {"parentheses", repeat("(", depth) + "1" + repeat(")", depth), false},
        {"unary", repeat("!", depth) + "a", false},
        {"arrays", repeat("[", depth) + repeat("]", depth), false},
        {"objects", repeat("{a:", depth) + "1" + repeat("}", depth), false},
        {"templates", repeat("`${", depth), false},
        {"calls", repeat("f(", depth) + repeat(")", depth), false},
        {"arrows", repeat("x=>", depth) + "x", false},
        {"conditionals", repeat("a?b:", depth) + "c", false},
        {"elements", repeat("<A>", depth), true},
        {"fragments", repeat("<>", depth), true},
        {"element in expression", repeat("<A b={", depth), true},
    };
    for (const auto& test : cases) {
        if (test.jsx) {
            const auto result = parse_jsx_element(test.source);
            check_eq(codes(result), "TESSERA_JSX_LIMIT_DEPTH;", "depth limit: " + test.name);
            check(result.root == nullptr, "no tree: " + test.name);
        } else {
            const auto result = parse_expression(test.source);
            check_eq(codes(result), "TESSERA_JSX_LIMIT_DEPTH;", "depth limit: " + test.name);
            check(result.root == nullptr, "no tree: " + test.name);
        }
    }
}

void test_depth_boundaries() {
    Limits small;
    small.max_nesting_depth = 8;
    check(parse_jsx_element(repeat("<A>", 8) + repeat("</A>", 8), small).ok(),
          "8 nested elements fit a limit of 8");
    check_eq(codes(parse_jsx_element(repeat("<A>", 9) + repeat("</A>", 9), small)),
             "TESSERA_JSX_LIMIT_DEPTH;", "9 nested elements do not");

    // Recursion depth is counted separately from tree height: prefix operators nest the parser
    // without making the tree taller than the operator count. One level for the expression
    // entry plus one per unary operator and one for the operand.
    check(parse_expression(repeat("!", 6) + "a", small).ok(),
          "6 prefix operators fit a limit of 8");
    check_eq(codes(parse_expression(repeat("!", 7) + "a", small)), "TESSERA_JSX_LIMIT_DEPTH;",
             "7 prefix operators do not");

    // A long chain is a deep tree, so it counts like nesting: that is what keeps deleting the
    // tree from overflowing the stack.
    const auto chain = [](std::size_t operands) { return "a" + repeat("+a", operands - 1); };
    check(parse_expression(chain(128)).ok(), "a chain of 128 operands fits the default limit");
    const auto too_long = parse_expression(chain(129));
    check_eq(codes(too_long), "TESSERA_JSX_LIMIT_DEPTH;", "129 operands do not");
    check(too_long.root == nullptr, "no tree for an over-deep chain");
    check_eq(codes(parse_expression(chain(5000))), "TESSERA_JSX_LIMIT_DEPTH;",
             "a very long chain stops at the limit");
    check_eq(codes(parse_expression("a" + repeat(".b", 5000))), "TESSERA_JSX_LIMIT_DEPTH;",
             "a very long member chain stops at the limit");
}

void test_huge_input() {
    const Limits limits;
    const auto oversized = parse_jsx_element(std::string(limits.max_file_bytes + 1, 'a'));
    check_eq(codes(oversized), "TESSERA_JSX_LIMIT_FILE_SIZE;", "file size limit");
    check(oversized.root == nullptr, "no tree for an oversized file");

    const std::string wrapper = "<A></A>";
    std::string exact = "<A>" + std::string(limits.max_file_bytes - wrapper.size(), 'x') + "</A>";
    check(exact.size() == limits.max_file_bytes, "test input is exactly at the limit");
    const auto at_limit = parse_jsx_element(exact);
    check(at_limit.ok(), "a file exactly at the limit is accepted");
    check(at_limit.root && at_limit.root->children.size() == 1 &&
              at_limit.root->children[0]->text.size() == limits.max_file_bytes - wrapper.size(),
          "one huge text node");

    Limits few_tokens;
    few_tokens.max_tokens = 1000;
    const auto tokens = parse_jsx_element("<>" + repeat("<A/>", 1000) + "</>", few_tokens);
    check_eq(codes(tokens), "TESSERA_JSX_LIMIT_TOKENS;",
             "token limit is reported once, without follow-on parse errors");
    check(tokens.root == nullptr, "no tree past the token limit");

    const auto flood = parse_jsx_element("<A>" + repeat("&bogus;", 300) + "</A>");
    check(flood.diagnostics.size() == limits.max_diagnostics + 1, "diagnostics are capped");
    check(flood.diagnostics_truncated, "truncation is flagged");
    check_eq(std::string(flood.diagnostics.back().code), "TESSERA_JSX_LIMIT_DIAGNOSTICS",
             "the cap ends with its own marker");
    check(flood.has_errors(), "errors stay known after truncation");

    std::string attributes = "<A";
    for (int i = 0; i < 2000; ++i) {
        attributes += " p" + std::to_string(i) + "=\"v\"";
    }
    attributes += "/>";
    const auto many = parse_jsx_element(attributes);
    check(many.ok() && many.root->attributes.size() == 2000, "2000 attributes");

    check(parse_expression("(" + repeat("a,", 4999) + "a) => a").ok(), "5000 arrow parameters");
    check_eq(codes(parse_expression("(" + repeat("a,", 4999) + "a)")),
             "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;", "a long parenthesized list is not an arrow");
}

void test_unterminated_constructs() {
    struct Case {
        const char* source;
        const char* first_code;
    };
    const Case cases[] = {
        {"<A b=\"x/>", "TESSERA_JSX_LEX_UNTERMINATED_STRING"},
        {"<A>{\"abc}</A>", "TESSERA_JSX_LEX_UNTERMINATED_STRING"},
        {"<A /* c", "TESSERA_JSX_LEX_UNTERMINATED_COMMENT"},
        {"<A>{`abc</A>", "TESSERA_JSX_LEX_UNTERMINATED_TEMPLATE"},
        {"<A>{foo", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN"},
        {"<A b={", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN"},
        {"<A b={x", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN"},
        {"<A b=", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN"},
        {"<A b", "TESSERA_JSX_PARSE_UNCLOSED_ELEMENT"},
        {"<A", "TESSERA_JSX_PARSE_UNCLOSED_ELEMENT"},
        {"<A>", "TESSERA_JSX_PARSE_UNCLOSED_ELEMENT"},
        {"<A></", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN"},
        {"<A></A", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN"},
        {"<A>&", "TESSERA_JSX_PARSE_UNCLOSED_ELEMENT"},
        {"<A>{", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN"},
        {"<A>{{", "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN"},
    };
    for (const auto& test : cases) {
        const auto result = parse_jsx_element(test.source);
        check(result.root == nullptr, std::string("no tree for ") + test.source);
        check(!result.diagnostics.empty() &&
                  std::string(first_diag(result).code) == test.first_code,
              std::string("first diagnostic for ") + test.source + " is " + test.first_code +
                  ", got " + codes(result));
    }
}

void test_invalid_bytes_do_not_stop_the_parse() {
    const auto utf8 = parse_jsx_element(std::string("<A b=\"\xFF\">\xC3(</A>"));
    check_eq(codes(utf8), "TESSERA_JSX_LEX_INVALID_UTF8;TESSERA_JSX_LEX_INVALID_UTF8;",
             "each bad byte is reported with its offset");
    check(utf8.root != nullptr && !utf8.ok() && utf8.diagnostics.size() == 2,
          "the tree is still built");
    check(utf8.diagnostics.size() == 2 && utf8.diagnostics[0].span.span.offset == 6 &&
              utf8.diagnostics[1].span.span.offset == 9,
          "offsets of the bad bytes");

    const auto nul = parse_jsx_element(std::string("<A>a\0b</A>", 10));
    check(nul.ok() && nul.root->children[0]->text.size() == 3, "NUL in text is ordinary text");
    check_eq(codes(parse_expression(std::string("a\0b", 3))),
             "TESSERA_JSX_LEX_INVALID_CHARACTER;"
             "TESSERA_JSX_PARSE_UNEXPECTED_TOKEN;",
             "NUL in code is an invalid character");

    check_eq(codes(parse_jsx_element("<A>\r</A>")), "TESSERA_JSX_LEX_LONE_CR;", "lone CR");
    check(parse_jsx_element("\xEF\xBB\xBF<A/>").ok(), "BOM before the element");
}

constexpr std::string_view kSample =
    "<View className=\"box\" style={{ width: 10 }} onClick={() => { go(1); }} {...props}>\n"
    "  <Text>Hello &amp; {name + `!${n}`}</Text>\n"
    "  {a ? <B/> : null}\n"
    "  {rows.map((r, i) => (<Row key={r.id} label={r.x === 'y'}/>))}\n"
    "  <><C d=\"1\"/></> // c\n"
    "</View>";

void test_every_truncation_and_mutation_has_a_tree_or_an_error() {
    const Limits limits;
    check(parse_jsx_element(kSample).ok(), "the sample itself is valid");

    for (std::size_t length = 0; length < kSample.size(); ++length) {
        const std::string_view prefix = kSample.substr(0, length);
        check_invariant(parse_jsx_element(prefix), limits, "jsx prefix " + std::to_string(length));
        check_invariant(parse_expression(prefix), limits, "expr prefix " + std::to_string(length));
    }
    constexpr std::string_view kMutations = "<>{}/\"'`$&(;=\n";
    for (std::size_t at = 0; at < kSample.size(); ++at) {
        for (const char replacement : kMutations) {
            std::string mutated(kSample);
            mutated[at] = replacement;
            check_invariant(parse_jsx_element(mutated), limits,
                            "jsx mutation at " + std::to_string(at));
            check_invariant(parse_expression(mutated), limits,
                            "expr mutation at " + std::to_string(at));
        }
        std::string deleted(kSample);
        deleted.erase(at, 1);
        check_invariant(parse_jsx_element(deleted), limits,
                        "jsx deletion at " + std::to_string(at));
    }
}

constexpr std::string_view kModuleSample =
    "import { useState, View } from \"tessera\";\n"
    "type P = { a: int; b?: Row[] } | null;\n"
    "const kMax: int = 10;\n"
    "export function A({ a = 1 }: P): Node {\n"
    "  const [n, setN] = useState<Array<int>>(a);\n"
    "  useEffect(() => { go(n); return () => { stop(); }; }, [n]);\n"
    "  return (<View onClick={() => setN(n + 1)}>{`t${n}`} &amp; x</View>);\n"
    "}\n"
    "function B() { return <A/>; }\n";

void test_module_inputs_always_give_a_module() {
    const Limits limits;
    const auto clean = parse_module(kModuleSample);
    check(clean.ok(), "the module sample is valid: " + codes(clean));

    // A module is produced for anything readable, with the damage confined to its items.
    const auto check_module = [&](const std::string& source, const std::string& what) {
        const auto result = parse_module(source);
        check(result.root != nullptr, "module present: " + what);
        check(result.diagnostics.size() <= limits.max_diagnostics + 1, "bounded: " + what);
        if (result.root && !result.has_errors()) {
            for (const auto& item : result.root->items) {
                check(item.valid, "no error means every item is valid: " + what);
            }
        }
    };
    for (std::size_t length = 0; length < kModuleSample.size(); ++length) {
        check_module(std::string(kModuleSample.substr(0, length)),
                     "prefix " + std::to_string(length));
    }
    constexpr std::string_view kMutations = "<>{}/\"'`$&(;=:[|\n";
    for (std::size_t at = 0; at < kModuleSample.size(); ++at) {
        for (const char replacement : kMutations) {
            std::string mutated(kModuleSample);
            mutated[at] = replacement;
            check_module(mutated, "mutation at " + std::to_string(at));
        }
    }
}

void test_module_level_limits_and_floods() {
    const std::size_t depth = 3000;
    check_eq(codes(parse_module("function A() { return " + repeat("<A>", depth))),
             "TESSERA_JSX_LIMIT_DEPTH;", "deep elements inside a component");
    check_eq(codes(parse_module("type T = " + repeat("A<", depth))), "TESSERA_JSX_LIMIT_DEPTH;",
             "deep generic type");
    check_eq(codes(parse_module("const a = " + repeat("(", depth))), "TESSERA_JSX_LIMIT_DEPTH;",
             "deep parentheses in a constant");
    check_eq(codes(parse_module("function A() { const [a] = " + repeat("f(", depth))),
             "TESSERA_JSX_LIMIT_DEPTH;", "deep calls in a statement");

    const auto many = parse_module(repeat("const a = 1;\n", 3000));
    check(many.ok() && many.root->items.size() == 3000, "3000 small items");

    const auto bad = parse_module(repeat("const = 1;\n", 3000));
    check(bad.root && bad.root->items.size() == 3000, "every bad item is kept");
    check(bad.diagnostics.size() == Limits{}.max_diagnostics + 1 && bad.diagnostics_truncated,
          "the report is capped");

    // Recovery must be linear: a long run of open braces before the next item.
    const auto braces = parse_module("function A() { " + repeat("{", 30000) + "\nconst b = 1;\n");
    check(braces.root && braces.root->items.size() == 2, "recovered after 30000 open braces");

    Limits few_tokens;
    few_tokens.max_tokens = 1000;
    const auto tokens = parse_module(repeat("const a=1;", 300), few_tokens);
    check_eq(codes(tokens), "TESSERA_JSX_LIMIT_TOKENS;", "token limit across a module");
}

void test_random_bytes() {
    const Limits limits;
    std::uint32_t state = 2024;
    for (int round = 0; round < 400; ++round) {
        std::string source = round % 2 == 0 ? "<A " : "a + ";
        for (int i = 0; i < 80; ++i) {
            state = state * 1664525U + 1013904223U;
            source.push_back(static_cast<char>(state >> 24));
        }
        check_invariant(parse_jsx_element(source), limits, "jsx random " + std::to_string(round));
        check_invariant(parse_expression(source), limits, "expr random " + std::to_string(round));
    }
}

} // namespace

int main() {
    test_deep_nesting_is_a_diagnostic();
    test_depth_boundaries();
    test_huge_input();
    test_unterminated_constructs();
    test_invalid_bytes_do_not_stop_the_parse();
    test_every_truncation_and_mutation_has_a_tree_or_an_error();
    test_module_inputs_always_give_a_module();
    test_module_level_limits_and_floods();
    test_random_bytes();
    return finish("jsx adversarial tests");
}
