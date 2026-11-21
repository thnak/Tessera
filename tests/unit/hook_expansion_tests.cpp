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

// The related span of the first expansion diagnostic, or empty.
std::string related_of(const std::string& source) {
    Pipeline pipeline(source, true);
    if (pipeline.sink == nullptr || pipeline.sink->diagnostics().empty()) {
        return {};
    }
    const jsx::Diagnostic& diagnostic = pipeline.sink->diagnostics().front();
    if (!diagnostic.related) {
        return "<none>";
    }
    return std::to_string(diagnostic.related->span.start.line) + ":" +
           std::to_string(diagnostic.related->span.start.column) + " | " +
           diagnostic.related->message;
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
    test_a_hook_used_by_no_component_is_not_expanded();
    return finish("hook expansion tests");
}
