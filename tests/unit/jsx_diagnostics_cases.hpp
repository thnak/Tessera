// SPDX-License-Identifier: MIT

#pragma once

// One minimal fixture per diagnostic code: the smallest source that reports exactly the code, the
// exact span it must report (byte offset, byte length, 1-based line), and the size of the whole
// report. The registry test runs every fixture and then compares the committed golden
// tests/golden/jsx/diagnostics-registry.txt, so a message or span change is a visible diff.
//
// Codes neither the front end nor the rules report yet are listed in kSkips with their owning
// stage, not silently omitted (tests/unit/jsx_diagnostics_registry_tests.cpp fails on an
// uncovered code).

#include "tessera/compiler/jsx/limits.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jsx_registry {

// Which entry point the fixture is fed to. kLex drives the Lexer directly, because a lexer
// diagnostic inside a larger parse would come with the parser's follow-up error. kExpression,
// kJsx, kType and kModule stop after parsing; kRules then runs the Tier-1 rules pass over the
// parsed module, so a rules code is reported by the stage that owns it.
enum class Entry { kLex, kExpression, kJsx, kType, kModule, kRules };

struct Case {
    std::string_view code;
    std::string_view name; // fixture id used by the golden, unique and stable
    Entry entry;
    std::string source;
    tessera::compiler::jsx::Limits limits{};
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t line = 1;
    std::size_t report_size = 1; // diagnostics the fixture must produce in total
};

// A code reserved for a later slice or owned by another stage. `owner` names the stage, `reason`
// says why no stage emits it yet.
struct Skip {
    std::string_view code;
    std::string_view owner;
    std::string_view reason;
};

namespace fixtures {

inline std::string repeat(std::string_view text, std::size_t count) {
    std::string out;
    out.reserve(text.size() * count);
    for (std::size_t i = 0; i < count; ++i) {
        out.append(text);
    }
    return out;
}

// Limits with only the limit under test narrowed, so a limit fixture stays a few hundred bytes
// while still reporting the production code and message shape.
inline tessera::compiler::jsx::Limits with_max_file_bytes(std::size_t bytes) {
    tessera::compiler::jsx::Limits limits;
    limits.max_file_bytes = bytes;
    return limits;
}

inline tessera::compiler::jsx::Limits with_max_tokens(std::size_t tokens) {
    tessera::compiler::jsx::Limits limits;
    limits.max_tokens = tokens;
    return limits;
}

} // namespace fixtures

// Compact table entry: the span is positional because every fixture pins it; the report size and
// the narrowed limits are the rare overrides and stay last.
inline Case fixture(std::string_view code, std::string_view name, Entry entry, std::string source,
                    std::uint32_t offset, std::uint32_t length, std::uint32_t line = 1,
                    std::size_t report_size = 1, tessera::compiler::jsx::Limits limits = {}) {
    return Case{code, name, entry, std::move(source), limits, offset, length, line, report_size};
}

// Sorted by code, mirroring the registry, so a reviewer can diff the two lists by eye.
inline const std::vector<Case> kCases = {
    fixture("TESSERA_JSX_LEX_INVALID_CHARACTER", "lex-invalid-character", Entry::kLex, "@", 0, 1),
    fixture("TESSERA_JSX_LEX_INVALID_ESCAPE", "lex-invalid-escape", Entry::kLex, "\"\\q\"", 1, 2),
    fixture("TESSERA_JSX_LEX_INVALID_NUMBER", "lex-invalid-number", Entry::kLex, "0x1F", 0, 4),
    fixture("TESSERA_JSX_LEX_INVALID_TEXT_CHARACTER", "lex-invalid-text-character", Entry::kJsx,
            "<A>}</A>", 3, 1),
    // A valid multi-byte code point before the bad byte keeps the column a code-point count.
    fixture("TESSERA_JSX_LEX_INVALID_UTF8", "lex-invalid-utf8", Entry::kLex, "\"\xC3\xA9\xFF\"", 3,
            1),
    fixture("TESSERA_JSX_LEX_LONE_CR", "lex-lone-cr", Entry::kLex, "a\rb", 1, 1),
    fixture("TESSERA_JSX_LEX_UNTERMINATED_COMMENT", "lex-unterminated-comment", Entry::kLex, "/*",
            0, 2),
    fixture("TESSERA_JSX_LEX_UNTERMINATED_STRING", "lex-unterminated-string", Entry::kLex, "\"abc",
            0, 1),
    fixture("TESSERA_JSX_LEX_UNTERMINATED_TEMPLATE", "lex-unterminated-template", Entry::kLex,
            "`abc", 0, 1),
    // The 65th component is parsed, then the limit is reported over its whole span.
    fixture("TESSERA_JSX_LIMIT_COMPONENTS", "limit-components", Entry::kModule,
            fixtures::repeat("function A(){return <A/>;}", 65), 1664, 26),
    // Each parenthesis costs two depth levels (expression and unary), so the 65th is the first
    // one over the default limit of 128.
    fixture("TESSERA_JSX_LIMIT_DEPTH", "limit-depth", Entry::kExpression,
            fixtures::repeat("(", 65) + "a", 64, 1),
    // 100 kept diagnostics, then the sink adds one cap entry; every further report is dropped.
    fixture("TESSERA_JSX_LIMIT_DIAGNOSTICS", "limit-diagnostics", Entry::kLex,
            fixtures::repeat("@", 101), 100, 1, 1, 101),
    fixture("TESSERA_JSX_LIMIT_FILE_SIZE", "limit-file-size", Entry::kModule, "12345678901234567",
            0, 0, 1, 1, fixtures::with_max_file_bytes(16)),
    fixture("TESSERA_JSX_LIMIT_TOKENS", "limit-tokens", Entry::kExpression, "a+b+c", 3, 1, 1, 1,
            fixtures::with_max_tokens(3)),
    fixture("TESSERA_JSX_PARSE_EMPTY_EXPRESSION", "parse-empty-expression", Entry::kJsx,
            "<A b={} />", 6, 1),
    fixture("TESSERA_JSX_PARSE_MISMATCHED_TAG", "parse-mismatched-tag", Entry::kJsx, "<A></B>", 3,
            4),
    fixture("TESSERA_JSX_PARSE_MISSING_RETURN", "parse-missing-return", Entry::kModule,
            "function A() { }", 15, 1),
    fixture("TESSERA_JSX_PARSE_MISSING_SEMICOLON", "parse-missing-semicolon", Entry::kModule,
            "function A() { return <A/> }", 26, 0),
    fixture("TESSERA_JSX_PARSE_UNCLOSED_ELEMENT", "parse-unclosed-element", Entry::kJsx, "<A>", 1,
            1),
    fixture("TESSERA_JSX_PARSE_UNEXPECTED_TOKEN", "parse-unexpected-token", Entry::kExpression, "+",
            1, 0),
    fixture("TESSERA_JSX_PARSE_UNKNOWN_ENTITY", "parse-unknown-entity", Entry::kJsx,
            "<A>&bogus;</A>", 3, 7),
    fixture("TESSERA_JSX_SUBSET_ARRAY_METHOD", "subset-array-method", Entry::kExpression,
            "rows.filter(x)", 0, 14),
    fixture("TESSERA_JSX_SUBSET_ASYNC", "subset-async", Entry::kExpression, "await x", 0, 5),
    fixture("TESSERA_JSX_SUBSET_CALL", "subset-call", Entry::kExpression, "rows.map(r => r)", 0,
            16),
    fixture("TESSERA_JSX_SUBSET_CLASS", "subset-class", Entry::kExpression, "new", 0, 3),
    fixture("TESSERA_JSX_SUBSET_COMPONENT_NAME", "subset-component-name", Entry::kModule,
            "function a() { return <A/>; }", 9, 1),
    fixture("TESSERA_JSX_SUBSET_DOM", "subset-dom", Entry::kJsx, "<A dangerouslySetInnerHTML={x}/>",
            3, 23),
    fixture("TESSERA_JSX_SUBSET_DYNAMIC_TAG", "subset-dynamic-tag", Entry::kJsx, "<A.B/>", 1, 3),
    fixture("TESSERA_JSX_SUBSET_FUNCTION_VALUE", "subset-function-value", Entry::kExpression,
            "function", 0, 8),
    fixture("TESSERA_JSX_SUBSET_HTML_TAG", "subset-html-tag", Entry::kJsx, "<div/>", 1, 3),
    fixture("TESSERA_JSX_SUBSET_IMPORT", "subset-import", Entry::kModule, "export default 1;", 7,
            7),
    fixture("TESSERA_JSX_SUBSET_LITERAL", "subset-literal", Entry::kExpression, "{[k]: 1}", 1, 1),
    fixture("TESSERA_JSX_SUBSET_LOOSE_EQUALITY", "subset-loose-equality", Entry::kExpression,
            "a == b", 2, 2),
    fixture("TESSERA_JSX_SUBSET_MEMBER", "subset-member", Entry::kExpression, "a[0]", 1, 1),
    fixture("TESSERA_JSX_SUBSET_MUTATION", "subset-mutation", Entry::kExpression, "a = 1", 2, 1),
    fixture("TESSERA_JSX_SUBSET_OPERATOR", "subset-operator", Entry::kExpression, "a ?? b", 2, 2),
    fixture("TESSERA_JSX_SUBSET_SPREAD", "subset-spread", Entry::kModule,
            "function A(props: P) { return <B {...other}/>; }\n", 33, 10),
    fixture("TESSERA_JSX_SUBSET_STATEMENT", "subset-statement", Entry::kExpression, "if", 0, 2),
    fixture("TESSERA_JSX_SUBSET_STYLE_DYNAMIC", "subset-style-dynamic", Entry::kJsx,
            "<A style={s}/>", 3, 9),
    fixture("TESSERA_JSX_SUBSET_TYPES", "subset-types", Entry::kType, "any", 0, 3),
    fixture("TESSERA_JSX_SUBSET_UNKNOWN_PROP", "subset-unknown-prop", Entry::kJsx,
            "<A data-x=\"1\"/>", 3, 6),
    fixture("TESSERA_JSX_TYPE_NUMBER", "type-number", Entry::kType, "number", 0, 6),
    fixture("TESSERA_REACT_BIND_ID", "react-bind-id", Entry::kModule,
            "function A() {\n"
            "  // Note: bind::Foo\n"
            "  const f = () => go();\n"
            "  return <B/>;\n"
            "}\n",
            17, 18, 2),
    fixture("TESSERA_REACT_BIND_MISSING", "react-bind-missing", Entry::kRules,
            "function A() {\n"
            "  const f = () => { go(); };\n"
            "  return <View/>;\n"
            "}\n",
            17, 26, 2),
    fixture("TESSERA_REACT_BIND_POSITION", "react-bind-position", Entry::kModule,
            "// Note: bind::x\nfunction A() { return <A/>; }\n", 0, 16),
    fixture("TESSERA_REACT_CUSTOM_HOOK", "react-custom-hook", Entry::kModule,
            "function useThing() { return <A/>; }", 9, 8),
    fixture("TESSERA_REACT_EFFECT_DEPS", "react-effect-deps", Entry::kRules,
            "function A() {\n"
            "  useEffect(() => { go(); });\n"
            "  return <View/>;\n"
            "}\n",
            17, 27, 2),
    fixture("TESSERA_REACT_EFFECT_RETURN", "react-effect-return", Entry::kRules,
            "function A() {\n"
            "  useEffect(() => { return 1; }, []);\n"
            "  return <View/>;\n"
            "}\n",
            35, 8, 2),
    fixture("TESSERA_REACT_HOOK_FORM", "react-hook-form", Entry::kRules,
            "function A() {\n"
            "  const x = useState<int>(0);\n"
            "  return <View/>;\n"
            "}\n",
            17, 27, 2),
    fixture("TESSERA_REACT_HOOK_ORDER", "react-hook-order", Entry::kRules,
            "function A() {\n"
            "  const a = count + 1;\n"
            "  const [count, setCount] = useState<int>(0);\n"
            "  return <View/>;\n"
            "}\n",
            27, 5, 2),
    fixture("TESSERA_REACT_LIST_KEY", "react-list-key", Entry::kExpression, "rows.map(r => <Row/>)",
            14, 6),
    fixture("TESSERA_REACT_NO_PREVIEW_BODY", "react-no-preview-body", Entry::kRules,
            "function A() {\n"
            "  // Note: bind::go\n"
            "  const go = (): void => {};\n"
            "  return <View onClick={go}/>;\n"
            "}\n",
            37, 26, 3),
    fixture("TESSERA_REACT_PREVIEW_BODY", "react-preview-body", Entry::kRules,
            "function A() {\n"
            "  // Note: bind::go\n"
            "  const go = (): void => { log(1); };\n"
            "  return <View onClick={go}/>;\n"
            "}\n",
            37, 35, 3),
    fixture("TESSERA_REACT_UNKNOWN_HOOK", "react-unknown-hook", Entry::kRules,
            "function A() {\n"
            "  const t = useTheme();\n"
            "  return <View/>;\n"
            "}\n",
            27, 8, 2),
    fixture("TESSERA_REACT_UNSUPPORTED_API", "react-unsupported-api", Entry::kRules,
            "function A() {\n"
            "  const m = useMemo(1);\n"
            "  return <View/>;\n"
            "}\n",
            27, 7, 2),
};

inline const std::vector<Skip> kSkips = {
    {"TESSERA_JSX_CHILDREN_NOT_ALLOWED", "schema",
     "declared as a constraint code in src/schema, but no pass evaluates element constraints yet"},
    {"TESSERA_JSX_SUBSET_ANIMATED_READ", "analyzer (later slice)",
     "reading an animated value needs the hook/type state of the later analysis slice"},
    {"TESSERA_JSX_SUBSET_IMPORT_CYCLE", "project resolver",
     "an import cycle spans files; the single-file parser cannot decide it"},
    {"TESSERA_JSX_TYPE_MISMATCH", "type/lowering",
     "needs declared prop, state and event types; no stage emits it yet"},
    {"TESSERA_REACT_HOOK_NOT_ALLOWED", "hook manifest",
     "reserved: it depends on ui.hooks.allowed, so no stage emits it yet"},
    {"TESSERA_REACT_SETTER_IN_RENDER", "analyzer (later slice)",
     "reserved: needs setter and call-site analysis; no stage emits it yet"},
};

} // namespace jsx_registry
