// SPDX-License-Identifier: MIT

// Hook rules (docs/specs/jsx-subset-v1.md section 7): symbol table, top-level placement,
// declaration order, unknown hooks and accepted forms.

#include "rules_test_util.hpp"

namespace {

using namespace rules_test;

const rules::ComponentAnalysis& only_component(const Fixture& fixture) {
    static const rules::ComponentAnalysis kEmpty;
    check(fixture.analyzed.components.size() == 1, "one component analysed");
    return fixture.analyzed.components.empty() ? kEmpty : fixture.analyzed.components[0];
}

void test_accepted_hooks_fill_the_symbol_table() {
    const auto fixture = analyze(component("  const [count, setCount] = useState<int>(0);\n"
                                           "  const r = useRef<View>(null);\n"
                                           "  const v = useAnimatedValue(0);\n"
                                           "  const doubled = count * 2;\n"
                                           "  // Note: bind::sync\n"
                                           "  useEffect(() => { log(count); }, [count]);\n",
                                           "<View/>", "{ label }: P"));
    check_eq(describe_all(*fixture), "", "no diagnostics for the accepted hook forms");
    const auto& analysis = only_component(*fixture);
    check(analysis.ok, "component is ok");
    check_eq(symbol_text(analysis),
             "label:Prop count:State setCount:StateSetter r:Ref v:AnimatedValue doubled:Local ",
             "symbols in declaration order, props first");
    check(analysis.find("setCount") != nullptr && analysis.find("setCount")->statement == 0,
          "the setter belongs to the useState statement");
    check(analysis.find("missing") == nullptr, "an undeclared name is not found");
    check(analysis.effects.size() == 1 && analysis.effects[0].bind_id == "sync",
          "the effect keeps its bind id");
}

void test_props_parameter_is_a_symbol() {
    const auto fixture = analyze(component("", "<View/>", "props: P"));
    check_eq(symbol_text(only_component(*fixture)), "props:PropsParam ",
             "a non-destructured props parameter is one symbol");
}

void test_hook_in_a_condition_is_rejected() {
    const auto ternary =
        analyze(component("  const [x, setX] = flag ? useState<int>(0) : useState<int>(1);\n"));
    check_eq(describe_all(*ternary),
             "TESSERA_REACT_HOOK_ORDER | 2:28 | useState<int>(0) | hook `useState` is called "
             "inside a condition; hooks are only allowed at the top level of the component body\n"
             "TESSERA_REACT_HOOK_ORDER | 2:47 | useState<int>(1) | hook `useState` is called "
             "inside a condition; hooks are only allowed at the top level of the component body\n",
             "both branches of a conditional hook are reported");
    check(!only_component(*ternary).ok, "the component is flagged");

    const auto logical = analyze(component("  const r = flag && useRef<View>(null);\n"));
    check_eq(describe_all(*logical),
             "TESSERA_REACT_HOOK_ORDER | 2:21 | useRef<View>(null) | hook `useRef` is called "
             "inside a condition; hooks are only allowed at the top level of the component body\n",
             "the right operand of && is a condition");

    const auto either = analyze(component("  const r = flag || useRef<View>(null);\n"));
    check(either->analyzed.diagnostics.size() == 1 &&
              either->analyzed.diagnostics[0].code == "TESSERA_REACT_HOOK_ORDER" &&
              either->analyzed.diagnostics[0].message.find("inside a condition") !=
                  std::string::npos,
          "the right operand of || is a condition");
}

void test_hook_in_a_function_is_rejected() {
    const auto fixture = analyze(component("  // Note: bind::go\n"
                                           "  const go = (): void => { useState<int>(0); };\n"));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_HOOK_ORDER | 3:28 | useState<int>(0) | hook `useState` is called "
             "inside a function (handler, effect or callback); hooks are only allowed at the "
             "top level of the component body\n",
             "a hook in a handler body");

    const auto effect = analyze(component("  useEffect(() => { useRef<View>(null); }, []);\n"));
    check_eq(describe_all(*effect),
             "TESSERA_REACT_HOOK_ORDER | 2:21 | useRef<View>(null) | hook `useRef` is called "
             "inside a function (handler, effect or callback); hooks are only allowed at the "
             "top level of the component body\n",
             "a hook in an effect body");
}

void test_hook_in_jsx_or_arguments_is_rejected() {
    const auto jsx_attr = analyze(component("", "<View a={useAnimatedValue(1)}/>"));
    check_eq(describe_all(*jsx_attr),
             "TESSERA_REACT_HOOK_ORDER | 2:19 | useAnimatedValue(1) | hook `useAnimatedValue` is "
             "called inside an expression; hooks are only allowed at the top level of the "
             "component body\n",
             "a hook in the returned element");
    const auto args =
        analyze(component("  const [x, setX] = useState<int>(useAnimatedValue(1));\n"));
    check_eq(describe_all(*args),
             "TESSERA_REACT_HOOK_ORDER | 2:35 | useAnimatedValue(1) | hook `useAnimatedValue` is "
             "called inside an expression; hooks are only allowed at the top level of the "
             "component body\n",
             "a hook in the argument of a hook");
}

void test_use_before_declaration_is_rejected() {
    const auto fixture = analyze(component("  const a = count + 1;\n"
                                           "  const [count, setCount] = useState<int>(0);\n"));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_HOOK_ORDER | 2:13 | count | `count` is used before the hook that "
             "declares it\n",
             "a local reads state declared by a later hook");

    const auto twice = analyze(component("  const a = count + count;\n"
                                         "  const [count, setCount] = useState<int>(0);\n"));
    check_eq(describe_all(*twice),
             "TESSERA_REACT_HOOK_ORDER | 2:13 | count | `count` is used before the hook that "
             "declares it\n",
             "one report per name and statement");

    const auto effect_first = analyze(component("  useEffect(() => { setCount(1); }, []);\n"
                                                "  const [count, setCount] = useState<int>(0);\n"));
    check_eq(describe_all(*effect_first),
             "TESSERA_REACT_HOOK_ORDER | 2:21 | setCount | `setCount` is used before the hook "
             "that declares it\n",
             "an effect calls a setter declared by a later hook (callee reads count)");

    const auto ordered = analyze(component("  const [count, setCount] = useState<int>(0);\n"
                                           "  const a = count + 1;\n"));
    check_eq(describe_all(*ordered), "", "declared first, used second");
}

void test_unknown_hooks_are_rejected() {
    const auto top = analyze(component("  const t = useTheme();\n"));
    check_eq(describe_all(*top),
             "TESSERA_REACT_UNKNOWN_HOOK | 2:13 | useTheme | unknown hook `useTheme`: only "
             "useState, useEffect, useRef and useAnimatedValue are available\n",
             "an unknown hook at the top level");
    const auto nested = analyze(component("", "<View a={useTheme()}/>"));
    check_eq(describe_all(*nested),
             "TESSERA_REACT_UNKNOWN_HOOK | 2:19 | useTheme | unknown hook `useTheme`: only "
             "useState, useEffect, useRef and useAnimatedValue are available\n",
             "an unknown hook nested in JSX is unknown first, misplaced second");
    const auto memo = analyze(component("  const m = useMemo(1);\n"));
    check_eq(describe_all(*memo),
             "TESSERA_REACT_UNSUPPORTED_API | 2:13 | useMemo | `useMemo` is not supported in "
             "V1\n",
             "React APIs that the subset names are unsupported, not unknown");
    for (const char* name : {"useCallback", "useReducer", "useContext"}) {
        const auto other = analyze(component(std::string("  const m = ") + name + "(1);\n"));
        check(other->analyzed.diagnostics.size() == 1 &&
                  other->analyzed.diagnostics[0].code == "TESSERA_REACT_UNSUPPORTED_API",
              std::string(name) + " is unsupported");
    }
}

void test_malformed_hook_forms_are_rejected() {
    const auto state = analyze(component("  const x = useState<int>(0);\n"));
    check_eq(describe_all(*state),
             "TESSERA_REACT_HOOK_FORM | 2:3 | const x = useState<int>(0); | `useState` is not "
             "written in its accepted form\n",
             "useState needs the pair pattern");
    check_eq(state->analyzed.diagnostics[0].suggestion,
             "write `const [x, setX] = useState<T>(init);`", "the suggestion shows the form");
    const auto three = analyze(component("  const [a, b, c] = useState<int>(0);\n"));
    check(three->analyzed.diagnostics.size() == 1 &&
              three->analyzed.diagnostics[0].code == "TESSERA_REACT_HOOK_FORM",
          "exactly two names");
    const auto bare = analyze(component("  useState<int>(0);\n"));
    check(bare->analyzed.diagnostics.size() == 1 &&
              bare->analyzed.diagnostics[0].code == "TESSERA_REACT_HOOK_FORM",
          "a hook used as a bare statement");
    const auto ref = analyze(component("  const r = useRef<View>(0);\n"));
    check_eq(describe_all(*ref),
             "TESSERA_REACT_HOOK_FORM | 2:3 | const r = useRef<View>(0); | `useRef` is not "
             "written in its accepted form\n",
             "the ref initial value must be null");
    const auto animated = analyze(component("  const [v, w] = useAnimatedValue(0);\n"));
    check(animated->analyzed.diagnostics.size() == 1 &&
              animated->analyzed.diagnostics[0].code == "TESSERA_REACT_HOOK_FORM",
          "an animated value is a single name");
    const auto effect = analyze(component("  useEffect(go, []);\n"));
    check_eq(describe_all(*effect),
             "TESSERA_REACT_HOOK_FORM | 2:3 | useEffect(go, []); | `useEffect` is not written "
             "in its accepted form\n",
             "the effect callback must be an arrow function");
    const auto effect_const = analyze(component("  const e = useEffect(() => { go(); }, []);\n"));
    check(effect_const->analyzed.diagnostics.size() == 1 &&
              effect_const->analyzed.diagnostics[0].code == "TESSERA_REACT_HOOK_FORM",
          "the value of useEffect is not used");
    const auto extra_arg = analyze(component("  useEffect(() => { go(); }, [], 1);\n"));
    check(extra_arg->analyzed.diagnostics.size() == 1 &&
              extra_arg->analyzed.diagnostics[0].code == "TESSERA_REACT_HOOK_FORM",
          "useEffect takes at most two arguments");
    const auto no_args = analyze(component("  useEffect();\n"));
    check(no_args->analyzed.diagnostics.size() == 1 &&
              no_args->analyzed.diagnostics[0].code == "TESSERA_REACT_HOOK_FORM",
          "useEffect takes a callback");
    const auto with_param = analyze(component("  useEffect((a) => { go(); }, []);\n"));
    check(with_param->analyzed.diagnostics.size() == 1 &&
              with_param->analyzed.diagnostics[0].code == "TESSERA_REACT_HOOK_FORM",
          "the effect callback takes no parameters");
}

void test_an_error_flags_only_its_component() {
    const auto fixture = analyze("function Bad() {\n  const t = useTheme();\n  return <View/>;\n}\n"
                                 "function Good() {\n  const [n, setN] = useState<int>(0);\n"
                                 "  return <View/>;\n}\n");
    check(fixture->analyzed.components.size() == 2, "two components analysed");
    check(!fixture->analyzed.components[0].ok, "the broken component is flagged");
    check(fixture->analyzed.components[1].ok, "the other component is unaffected");
    check(fixture->analyzed.has_errors(), "the module reports an error");
    check(fixture->analyzed.find_component(0) == &fixture->analyzed.components[0] &&
              fixture->analyzed.find_component(1) == &fixture->analyzed.components[1] &&
              fixture->analyzed.find_component(2) == nullptr,
          "components are found by item index");
}

void test_parser_invalid_items_are_skipped() {
    const std::string source =
        "function useThing() {\n  const t = useTheme();\n  return <View/>;\n}\n"
        "type T = { a: int };\n";
    const auto parsed = jsx::parse_module(source);
    check(!parsed.ok() && parsed.root != nullptr, "the custom hook is a parser error");
    const jsx::SourceMap map(source);
    const auto analyzed = rules::analyze_module(*parsed.root, map);
    check(analyzed.components.size() == 1, "the invalid component is listed");
    check(!analyzed.components[0].ok, "it is flagged");
    check(analyzed.diagnostics.empty(), "analysis adds no diagnostics for an invalid item");
    check(analyzed.components[0].symbols.empty(), "nothing was analysed");
}

} // namespace

int main() {
    test_accepted_hooks_fill_the_symbol_table();
    test_props_parameter_is_a_symbol();
    test_hook_in_a_condition_is_rejected();
    test_hook_in_a_function_is_rejected();
    test_hook_in_jsx_or_arguments_is_rejected();
    test_use_before_declaration_is_rejected();
    test_unknown_hooks_are_rejected();
    test_malformed_hook_forms_are_rejected();
    test_an_error_flags_only_its_component();
    test_parser_invalid_items_are_skipped();
    return finish("rules hooks tests");
}
