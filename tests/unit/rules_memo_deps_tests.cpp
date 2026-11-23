// SPDX-License-Identifier: MIT

// `useMemo`/`useCallback` dependency lists are compared with the inferred reactive set, not
// required to match it exactly (issue #829; docs/specs/jsx-intrinsics-v1/react-compat.md
// section 4.2). A disagreement is a warning that names what to fix; a non-name element is an error,
// the same rule `useEffect` has. An absent array is accepted.

#include "rules_test_util.hpp"

namespace {

using namespace rules_test;

const std::string kHooks = "  const [count, setCount] = useState<int>(0);\n"
                           "  const [name, setName] = useState(\"a\");\n";

void test_exact_list_is_accepted() {
    const auto memo = analyze(component(kHooks + "  const label = useMemo(() => count + name, "
                                                 "[count, name]);\n"));
    check_eq(describe_all(*memo), "", "a memo over exactly what it lists has no diagnostic");
    check(memo->analyzed.components[0].ok, "and the component is fine");

    const auto callback =
        analyze(component("  const [on, setOn] = useState<boolean>(false);\n"
                          "  const toggle = useCallback(() => setOn(!on), [on]);\n",
                          "<View onClick={toggle}/>"));
    check_eq(describe_all(*callback), "", "an exact callback list has no diagnostic");
    check(callback->analyzed.components[0].ok, "and the component is fine");
}

void test_missing_dependency_warns() {
    const auto fixture =
        analyze(component(kHooks + "  const label = useMemo(() => count + name, [count]);\n"));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_MEMO_DEPS | 4:45 | [count] | `name` is read but not listed; Tessera "
             "recomputes when it changes, React would keep the old value\n",
             "reported on the array, naming the name read but not listed");
    check_eq(fixture->analyzed.diagnostics[0].suggestion, "add `name` to the dependency array",
             "the suggestion says what to add");
    check(fixture->analyzed.diagnostics[0].severity == jsx::Severity::Warning,
          "a missing dependency is a warning");
    check(fixture->analyzed.components[0].ok, "a warning does not fail the component");
}

void test_extra_dependency_warns() {
    const auto fixture =
        analyze(component(kHooks + "  const twice = useMemo(() => count * 2, [count, name]);\n"));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_MEMO_DEPS | 4:50 | name | `name` is listed but the body does not read "
             "it\n",
             "reported on the extra entry");
    check_eq(fixture->analyzed.diagnostics[0].suggestion, "remove `name` from the dependency array",
             "the suggestion says what to remove");
    check(fixture->analyzed.diagnostics[0].severity == jsx::Severity::Warning,
          "an extra dependency is a warning");
    check(fixture->analyzed.components[0].ok, "a warning does not fail the component");
}

void test_non_plain_name_errors() {
    const auto fixture =
        analyze(component("  const [count, setCount] = useState<int>(0);\n"
                          "  const twice = useMemo(() => count * 2, [count + 1]);\n"));
    // The element that is not a name is the error; `count` is still read and unlisted, so the
    // missing-name warning follows, as `useEffect` reports after its non-name error.
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_MEMO_DEPS | 3:43 | count + 1 | a memo dependency must be a plain name, "
             "not an expression\n"
             "TESSERA_REACT_MEMO_DEPS | 3:42 | [count + 1] | `count` is read but not listed; "
             "Tessera recomputes when it changes, React would keep the old value\n",
             "a non-name element is an error and the name it stood for is still missing");
    check(fixture->analyzed.diagnostics[0].severity == jsx::Severity::Error,
          "a non-name element is an error");
    check(!fixture->analyzed.components[0].ok, "the error fails the component");
}

void test_absent_array_is_accepted() {
    const auto without = analyze(component(kHooks + "  const twice = useMemo(() => count * 2);\n"));
    check_eq(describe_all(*without), "",
             "a memo with no array is accepted (issue #829 decision 3)");

    const auto empty =
        analyze(component(kHooks + "  const twice = useMemo(() => count * 2, []);\n"));
    check_eq(describe_all(*empty),
             "TESSERA_REACT_MEMO_DEPS | 4:42 | [] | `count` is read but not listed; Tessera "
             "recomputes when it changes, React would keep the old value\n",
             "an empty list over a reading body warns; it is not an error");
    check(empty->analyzed.diagnostics[0].severity == jsx::Severity::Warning,
          "the empty list still only warns");
}

void test_inferred_deps_resolve_through_a_local() {
    // A memo over another memo resolves through to the state the inner memo reads, so the outer
    // list must name `count` (the base memo is a local, not a dependency).
    const auto fixture = analyze(component("  const [count, setCount] = useState<int>(0);\n"
                                           "  const base = useMemo(() => count + 1, [count]);\n"
                                           "  const twice = useMemo(() => base * 2, [count]);\n"));
    check_eq(describe_all(*fixture), "", "the outer memo lists the state the inner one reads");

    const auto missing = analyze(component("  const [count, setCount] = useState<int>(0);\n"
                                           "  const base = useMemo(() => count + 1, [count]);\n"
                                           "  const twice = useMemo(() => base * 2, []);\n"));
    check_eq(describe_all(*missing),
             "TESSERA_REACT_MEMO_DEPS | 4:41 | [] | `count` is read but not listed; Tessera "
             "recomputes when it changes, React would keep the old value\n",
             "the dependency is reported under the state, not the local that reads it");
}

void test_prop_is_a_dependency() {
    const auto exact = analyze(component("  const n = useMemo(() => label.length, [label]);\n",
                                         "<View/>", "{ label }: P"));
    check_eq(describe_all(*exact), "", "a destructured prop is a dependency by name");

    const auto missing = analyze(
        component("  const n = useMemo(() => label.length, []);\n", "<View/>", "{ label }: P"));
    check_eq(describe_all(*missing),
             "TESSERA_REACT_MEMO_DEPS | 2:41 | [] | `label` is read but not listed; Tessera "
             "recomputes when it changes, React would keep the old value\n",
             "a missing prop dependency warns");
}

void test_memo_inside_a_custom_hook_follows_the_rename() {
    // A memo written inside a project hook records the hook's own names; the expansion renames them
    // before the rules pass, so the check must compare the renamed list, not the source one.
    const std::string source = "function useDouble(step: int) {\n"
                               "  const doubled = useMemo(() => step * 2, [step]);\n"
                               "  return { doubled };\n"
                               "}\n"
                               "function A({ count }: P) {\n"
                               "  const t = useDouble(count);\n"
                               "  return <View style={{ width: t.doubled }}/>;\n"
                               "}\n";
    jsx::FrontEndOptions options;
    options.custom_hooks = true;
    jsx::ModuleParseResult parsed = jsx::parse_module(source, jsx::Limits{}, options);
    check(parsed.ok(), "the hook fixture parses: " + codes(parsed));
    if (!parsed.root) {
        return;
    }
    const jsx::SourceMap map(source);
    jsx::DiagnosticSink sink(map, jsx::Limits{});
    const jsx::ExpansionMetrics metrics =
        jsx::expand_custom_hooks(*parsed.root, map, options, sink);
    const rules::AnalyzedModule analyzed =
        rules::analyze_module(*parsed.root, map, jsx::Limits{}, nullptr, &metrics.call_sites);
    std::string all;
    for (const auto& diagnostic : analyzed.diagnostics) {
        all += std::string(diagnostic.code) + " | " + diagnostic.message + "\n";
    }
    check_eq(all, "", "the memo's `step` renames to `count`, so the expanded list still matches");
}

} // namespace

int main() {
    test_exact_list_is_accepted();
    test_missing_dependency_warns();
    test_extra_dependency_warns();
    test_non_plain_name_errors();
    test_absent_array_is_accepted();
    test_inferred_deps_resolve_through_a_local();
    test_prop_is_a_dependency();
    test_memo_inside_a_custom_hook_follows_the_rename();
    return finish("rules memo deps tests");
}
