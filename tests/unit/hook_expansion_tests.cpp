// SPDX-License-Identifier: MIT

// The rules of the custom-hook expansion (spike #659, ADR-065; docs/specs/custom-hooks-v1.md
// section 4) and its diagnostics (section 9): renaming, parameter binding, result substitution,
// the ordinal, the depth and cycle limits, and the related span that points at the call site.

#include "hook_expansion_test_util.hpp"

#include <string>
#include <vector>

namespace {

using namespace hook_expansion_test;

// The statements of the first component after expansion, as the module dump spells them.
std::string expanded_body(const std::string& source) {
    Pipeline pipeline(source, true);
    check(pipeline.parsed.ok(), "the fixture parses: " + first_diagnostic(pipeline));
    if (!pipeline.parsed.ok()) {
        return {};
    }
    return jsx::dump(*pipeline.parsed.root);
}

// The first diagnostic the expansion itself reported, or empty.
std::string expansion_diagnostic(const std::string& source) {
    Pipeline pipeline(source, true);
    if (pipeline.sink == nullptr || pipeline.sink->diagnostics().empty()) {
        return {};
    }
    const jsx::Diagnostic& diagnostic = pipeline.sink->diagnostics().front();
    return std::string(diagnostic.code) + " | " + std::to_string(diagnostic.span.start.line) + ":" +
           std::to_string(diagnostic.span.start.column) + " | " + diagnostic.message;
}

void test_renaming_uses_the_call_site_prefix() {
    const std::string source = "function useThing() {\n"
                               "  const [n, setN] = useState<int>(0);\n"
                               "  const bump = () => setN(n + 1);\n"
                               "  return { n, bump };\n"
                               "}\n"
                               "function A() {\n"
                               "  const t = useThing();\n"
                               "  return <View a={t.n} onClick={t.bump}/>;\n"
                               "}\n";
    const std::string dump = expanded_body(source);
    check(dump.find("useThing_1__n") != std::string::npos, "the state is renamed per call site");
    check(dump.find("useThing_1__setN") != std::string::npos, "the setter is renamed too");
    check(dump.find("useThing_1__bump") != std::string::npos, "the handler is renamed too");
    check(dump.find("(const t ") == std::string::npos, "the call statement is gone");
}

void test_two_calls_get_two_ordinals() {
    const std::string source = "function useThing() {\n"
                               "  const [n, setN] = useState<int>(0);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A() {\n"
                               "  const a = useThing();\n"
                               "  const b = useThing();\n"
                               "  return <View a={a.n} b={b.n}/>;\n"
                               "}\n";
    const std::string dump = expanded_body(source);
    check(dump.find("useThing_1__n") != std::string::npos, "the first call is ordinal 1");
    check(dump.find("useThing_2__n") != std::string::npos, "the second call is ordinal 2");
}

void test_a_plain_name_argument_is_substituted() {
    const std::string source = "function useThing(start: int) {\n"
                               "  const [n, setN] = useState<int>(start);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A(props: P) {\n"
                               "  const t = useThing(props.start);\n"
                               "  return <View a={t.n}/>;\n"
                               "}\n";
    const std::string dump = expanded_body(source);
    check(dump.find("(const useThing_1__start ") == std::string::npos,
          "a plain-name argument is not copied into a local");
    check(dump.find("(id useState) <int> (member start (id props))") != std::string::npos,
          "the argument is substituted by name: " + dump);
}

void test_an_expression_argument_becomes_a_local() {
    const std::string source = "function useThing(start: int) {\n"
                               "  const [n, setN] = useState<int>(start);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A(props: P) {\n"
                               "  const t = useThing(props.start + 1);\n"
                               "  return <View a={t.n}/>;\n"
                               "}\n";
    const std::string dump = expanded_body(source);
    check(dump.find("(const useThing_1__start (binary + (member start (id props)) (num 1)))") !=
              std::string::npos,
          "an expression argument becomes a const local of the prefixed name: " + dump);
}

void test_a_callback_argument_is_substituted() {
    const std::string source = "function useThing(onRun: () => void) {\n"
                               "  const go = () => onRun();\n"
                               "  return { go };\n"
                               "}\n"
                               "function A() {\n"
                               "  // Note: bind::go\n"
                               "  const go = (): void => { log(1); };\n"
                               "  const t = useThing(go);\n"
                               "  return <View onClick={t.go}/>;\n"
                               "}\n";
    const std::string dump = expanded_body(source);
    check(dump.find("(const useThing_1__go (arrow () (call (id go))))") != std::string::npos,
          "the callback parameter is replaced by the handler reference: " + dump);
}

void test_a_whole_value_binding_is_rejected() {
    const std::string source = "function useThing() {\n"
                               "  const [n, setN] = useState<int>(0);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A() {\n"
                               "  const t = useThing();\n"
                               "  return <View a={t}/>;\n"
                               "}\n";
    const std::string diagnostic = expansion_diagnostic(source);
    check(diagnostic.starts_with("TESSERA_REACT_HOOK_RETURN"), "a whole-value use is rejected");
}

void test_an_unknown_field_is_rejected() {
    const std::string source = "function useThing() {\n"
                               "  const [n, setN] = useState<int>(0);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A() {\n"
                               "  const { missing } = useThing();\n"
                               "  return <View a={missing}/>;\n"
                               "}\n";
    const std::string diagnostic = expansion_diagnostic(source);
    check(diagnostic.starts_with("TESSERA_REACT_HOOK_RETURN"),
          "an object pattern field the hook does not return is rejected");
}

void test_a_cycle_is_rejected() {
    const std::string source = "function useA() {\n"
                               "  const a = useB();\n"
                               "  return { a };\n"
                               "}\n"
                               "function useB() {\n"
                               "  const b = useA();\n"
                               "  return { b };\n"
                               "}\n"
                               "function A() {\n"
                               "  const v = useA();\n"
                               "  return <View a={v.a}/>;\n"
                               "}\n";
    const std::string diagnostic = expansion_diagnostic(source);
    check(diagnostic.starts_with("TESSERA_REACT_HOOK_CYCLE"), "a cycle is rejected");
}

void test_depth_is_limited() {
    std::string source;
    for (int i = 0; i < 10; ++i) {
        source += "function useH" + std::to_string(i) + "() {\n";
        source += "  const v = useH" + std::to_string(i + 1) + "();\n";
        source += "  return { v };\n";
        source += "}\n";
    }
    source += "function useH10() {\n  const [n, setN] = useState<int>(0);\n  return { n };\n}\n";
    source += "function A() {\n  const v = useH0();\n  return <View a={v.v}/>;\n}\n";
    const std::string diagnostic = expansion_diagnostic(source);
    check(diagnostic.starts_with("TESSERA_REACT_HOOK_DEPTH"),
          "expansion deeper than 8 is rejected");
}

// Spec section 8: a diagnostic whose primary span is inside a hook carries the call site as its
// related span. The expansion's own diagnostics are reported at the call site, so the related span
// is what the *rules* attach; this checks the map the rules read.
void test_the_call_site_map_names_the_expansion() {
    const std::string source = "function useThing() {\n"
                               "  const [n, setN] = useState<int>(0);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A() {\n"
                               "  const t = useThing();\n"
                               "  return <View a={t.n}/>;\n"
                               "}\n";
    Pipeline pipeline(source, true);
    check(pipeline.parsed.ok(), "the fixture parses");
    const jsx::CallSiteMap& sites = pipeline.metrics.call_sites;
    check(sites.regions.size() == 1, "one call site was recorded");
    if (sites.regions.empty()) {
        return;
    }
    const jsx::ExpandedRegion& region = sites.regions.front();
    check_eq(region.hook, "useThing_1", "the region names the expansion");
    const jsx::ResolvedSpan call = pipeline.map.resolve(region.call_span);
    check_eq(std::to_string(call.start.line) + ":" + std::to_string(call.start.column), "6:3",
             "the call site is the call statement");
    // A span inside the spliced statements maps back to that call site.
    const jsx::ExpandedRegion* found = sites.find(region.span);
    check(found != nullptr && found->hook == "useThing_1",
          "a span inside the hook maps to its call site");
    check(sites.find(jsx::Span{0, 1}) == nullptr, "a span outside every hook maps to nothing");
}

// The expansion's own diagnostic at the call site needs no related span: it already points there.
void test_a_call_site_diagnostic_has_no_related_span() {
    const std::string source = "function useThing() {\n"
                               "  const [n, setN] = useState<int>(0);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A() {\n"
                               "  const t = useThing();\n"
                               "  return <View a={t}/>;\n"
                               "}\n";
    Pipeline pipeline(source, true);
    check(pipeline.sink != nullptr && !pipeline.sink->diagnostics().empty(),
          "the whole-value use is rejected");
    if (pipeline.sink == nullptr || pipeline.sink->diagnostics().empty()) {
        return;
    }
    const jsx::Diagnostic& diagnostic = pipeline.sink->diagnostics().front();
    check_eq(std::string(diagnostic.code), "TESSERA_REACT_HOOK_RETURN", "the code");
    check_eq(std::to_string(diagnostic.span.start.line) + ":" +
                 std::to_string(diagnostic.span.start.column),
             "7:19", "the primary span is the whole-value use");
}

// A diagnostic raised INSIDE the hook body points at the hook and carries the call site as its
// related span (docs/specs/custom-hooks-v1.md section 8). The cycle is detected while expanding
// `useB`'s body, so its primary span is the `useA()` statement on line 6 — inside a function the
// reader did not write at that site — and the related span is the `useA()` call the component makes
// on line 10, which is the line they can act on.
void test_a_hook_body_diagnostic_carries_the_call_site() {
    const std::string source = "function useA() {\n"
                               "  const a = useB();\n"
                               "  return { a };\n"
                               "}\n"
                               "function useB() {\n"
                               "  const b = useA();\n"
                               "  return { b };\n"
                               "}\n"
                               "function A() {\n"
                               "  const v = useA();\n"
                               "  return <View a={v.a}/>;\n"
                               "}\n";
    Pipeline pipeline(source, true);
    check(pipeline.sink != nullptr && !pipeline.sink->diagnostics().empty(),
          "the cycle is rejected");
    if (pipeline.sink == nullptr || pipeline.sink->diagnostics().empty()) {
        return;
    }
    const jsx::Diagnostic& diagnostic = pipeline.sink->diagnostics().front();
    check_eq(std::string(diagnostic.code), "TESSERA_REACT_HOOK_CYCLE", "the code");
    check_eq(std::to_string(diagnostic.span.start.line) + ":" +
                 std::to_string(diagnostic.span.start.column),
             "6:3", "the primary span is the statement inside the hook body");
    check(diagnostic.related.has_value(), "the diagnostic carries a related span");
    if (!diagnostic.related) {
        return;
    }
    check_eq(std::to_string(diagnostic.related->span.start.line) + ":" +
                 std::to_string(diagnostic.related->span.start.column),
             "10:3", "the related span is the call site in the component");
    check_eq(diagnostic.related->message, "expanded here, from useA_1",
             "the related span names the expansion");
}

// The wiring this slice adds: a diagnostic the RULES raise inside an expanded hook body carries the
// call site too. The effect in `useWatch` reads the hook's own state but lists no dependency, which
// is `TESSERA_REACT_EFFECT_DEPS` — reported by the rules pass, at the array inside the hook body.
void test_a_rules_diagnostic_in_a_hook_body_carries_the_call_site() {
    const std::string source = "function useWatch() {\n"
                               "  const [n, setN] = useState<int>(0);\n"
                               "  useEffect(() => { log(n); }, []);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A() {\n"
                               "  const w = useWatch();\n"
                               "  return <View a={w.n}/>;\n"
                               "}\n";
    Pipeline pipeline(source, true);
    check(pipeline.parsed.ok(), "the fixture parses");
    const jsx::Diagnostic* found = nullptr;
    for (const jsx::Diagnostic& diagnostic : pipeline.analyzed.diagnostics) {
        if (diagnostic.code == "TESSERA_REACT_EFFECT_DEPS") {
            found = &diagnostic;
            break;
        }
    }
    check(found != nullptr, "the rules report the missing dependency");
    if (found == nullptr) {
        return;
    }
    check_eq(std::to_string(found->span.start.line), "3",
             "the primary span is the dependency array inside the hook body");
    check(found->related.has_value(), "the rules diagnostic carries a related span");
    if (!found->related) {
        return;
    }
    check_eq(std::to_string(found->related->span.start.line) + ":" +
                 std::to_string(found->related->span.start.column),
             "7:3", "the related span is the call site in the component");
    check_eq(found->related->message, "expanded here, from useWatch_1",
             "the related span names the expansion");
}

// A diagnostic in the component's own source is not inside any expanded region, so it gains no
// related span: the map must not invent one.
void test_a_component_diagnostic_has_no_related_span() {
    const std::string source = "function useThing() {\n"
                               "  const [n, setN] = useState<int>(0);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A() {\n"
                               "  const w = useThing();\n"
                               "  useEffect(() => { log(w.n); }, []);\n"
                               "  return <View a={w.n}/>;\n"
                               "}\n";
    Pipeline pipeline(source, true);
    check(pipeline.parsed.ok(), "the fixture parses");
    const jsx::Diagnostic* found = nullptr;
    for (const jsx::Diagnostic& diagnostic : pipeline.analyzed.diagnostics) {
        if (diagnostic.code == "TESSERA_REACT_EFFECT_DEPS") {
            found = &diagnostic;
            break;
        }
    }
    check(found != nullptr, "the rules report the missing dependency");
    if (found == nullptr) {
        return;
    }
    check_eq(std::to_string(found->span.start.line), "7",
             "the primary span is the component's own effect");
    check(!found->related.has_value(),
          "a diagnostic outside every expanded region has no related span");
}

// The expansion is wired into the PRODUCTION pipeline, not only into this test's harness. Before
// this landing `expand_custom_hooks` was called from nowhere but `hook_expansion_test_util.hpp`, so
// the pass was dead code in every real run: a project with a custom hook would have been analysed
// with the hook call still in place. `SourceFile` (the codegen driver's per-file stage) now runs
// parse -> expand -> analyse, which is the order the spec requires.
//
// This test drives that stage directly, so it fails if the wiring is ever removed.
void test_the_production_pipeline_expands() {
    // With the switch OFF the same source is rejected exactly as before: the wiring must not
    // change the default behaviour.
    const std::string source = "function useThing() {\n"
                               "  const [n, setN] = useState<int>(0);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A() {\n"
                               "  const t = useThing();\n"
                               "  return <View a={t.n}/>;\n"
                               "}\n";
    {
        const Pipeline off(source, false);
        check(!off.parsed.diagnostics.empty(),
              "with the switch off a custom hook is still rejected");
    }
    {
        const Pipeline on(source, true);
        check(on.ok(), "with the switch on the production stage expands and analyses cleanly");
        check(on.metrics.call_sites.regions.size() == 1,
              "the production stage recorded the call site");
    }
}

void test_a_hook_used_by_no_component_is_not_expanded() {
    const std::string source = "function useThing() {\n"
                               "  const [n, setN] = useState<int>(0);\n"
                               "  return { n, setN };\n"
                               "}\n"
                               "function A() {\n"
                               "  return <View/>;\n"
                               "}\n";
    Pipeline pipeline(source, true);
    check(pipeline.parsed.ok(), "the fixture parses");
    check(pipeline.metrics.added_statements.size() == 1 &&
              pipeline.metrics.added_statements.front() == 0,
          "a component that calls no hook adds no statement");
}

} // namespace

// Spec section 8, "Preview": the preview carries state across an edit by id, and an expanded id
// includes the hook name and the ordinal, so state survives an edit that does not reorder or rename
// the hook calls. This pins the property the preview relies on.
void test_state_ids_are_stable_across_an_edit() {
    const std::string before = "function useFlag(initial: boolean) {\n"
                               "  const [on, setOn] = useState<boolean>(initial);\n"
                               "  return { on, setOn };\n"
                               "}\n"
                               "function A() {\n"
                               "  const a = useFlag(true);\n"
                               "  const b = useFlag(false);\n"
                               "  return <View a={a.on} b={b.on}/>;\n"
                               "}\n";
    // The same call order, with an unrelated edit (a prop added to the returned element).
    const std::string after = "function useFlag(initial: boolean) {\n"
                              "  const [on, setOn] = useState<boolean>(initial);\n"
                              "  return { on, setOn };\n"
                              "}\n"
                              "function A() {\n"
                              "  const a = useFlag(true);\n"
                              "  const b = useFlag(false);\n"
                              "  return <View a={a.on} b={b.on} c={1}/>;\n"
                              "}\n";
    Pipeline first(before, true);
    Pipeline second(after, true);
    check(first.parsed.ok() && second.parsed.ok(), "both versions parse");
    const std::string one = dump(*first.parsed.root);
    const std::string two = dump(*second.parsed.root);
    check(one.find("useFlag_1__on") != std::string::npos, "the first call site has its own id");
    check(one.find("useFlag_2__on") != std::string::npos, "the second call site has its own id");
    check(two.find("useFlag_1__on") != std::string::npos &&
              two.find("useFlag_2__on") != std::string::npos,
          "an edit that keeps the call order keeps both ids, so the preview keeps their state");

    // A reorder changes the ids, which is the honest limit of the rule: the preview cannot tell a
    // reorder from a different hook, so it must not carry state across one.
    const std::string reordered = "function useFlag(initial: boolean) {\n"
                                  "  const [on, setOn] = useState<boolean>(initial);\n"
                                  "  return { on, setOn };\n"
                                  "}\n"
                                  "function A() {\n"
                                  "  const b = useFlag(false);\n"
                                  "  const a = useFlag(true);\n"
                                  "  return <View a={a.on} b={b.on}/>;\n"
                                  "}\n";
    Pipeline third(reordered, true);
    check(third.parsed.ok(), "the reordered version parses");
    const std::string three = dump(*third.parsed.root);
    check(three.find("useFlag_1__on") != std::string::npos,
          "the ordinal follows the call order, not the variable name");
}

int main() {
    test_renaming_uses_the_call_site_prefix();
    test_two_calls_get_two_ordinals();
    test_a_plain_name_argument_is_substituted();
    test_an_expression_argument_becomes_a_local();
    test_a_callback_argument_is_substituted();
    test_a_whole_value_binding_is_rejected();
    test_an_unknown_field_is_rejected();
    test_a_cycle_is_rejected();
    test_depth_is_limited();
    test_the_call_site_map_names_the_expansion();
    test_a_call_site_diagnostic_has_no_related_span();
    test_a_hook_body_diagnostic_carries_the_call_site();
    test_a_rules_diagnostic_in_a_hook_body_carries_the_call_site();
    test_a_component_diagnostic_has_no_related_span();
    test_the_production_pipeline_expands();
    test_a_hook_used_by_no_component_is_not_expanded();
    test_state_ids_are_stable_across_an_edit();
    return finish("hook expansion tests");
}
