// SPDX-License-Identifier: MIT

// Arrow roles, `return` placement in effects and the un-annotated declared arrow rule
// (docs/specs/jsx-subset-v1.md sections 7 and 9, docs/design/jsx-ir-lowering.md decision 4).

#include "rules_test_util.hpp"

namespace {

using namespace rules_test;

const std::string kState = "  const [count, setCount] = useState<int>(0);\n"
                           "  const v = useAnimatedValue(0);\n";

void test_roles_of_every_arrow() {
    const auto fixture = analyze(component(
        kState + "  // Note: bind::on_pick\n"
                 "  const pick = (id: int): void => { setCount(id); };\n"
                 "  const bump = () => { setCount(count + 1); v.set(1); };\n"
                 "  const reset = () => setCount(0);\n"
                 "  // Note: bind::sync\n"
                 "  useEffect(() => { log(count); return () => { stop(); }; }, [count]);\n",
        "<View onClick={() => setCount(1)} onTap={bump}>{rows.map((r) => <Row key={r}/>)}</View>"));
    check_eq(describe_all(*fixture), "", "no diagnostics");
    const auto& analysis = fixture->analyzed.components[0];
    check_eq(
        role_text(analysis),
        "NativeHandler(on_pick) InlineHandler InlineHandler Effect(sync) Cleanup(sync_cleanup) "
        "EventArrow ListCallback ",
        "roles in source order, with bind ids; the cleanup id is the effect id + _cleanup");
    check(analysis.effects.size() == 1 && analysis.effects[0].cleanup != nullptr,
          "the effect records its cleanup arrow");
    check(analysis.find_arrow(analysis.effects[0].cleanup) != nullptr &&
              analysis.find_arrow(analysis.effects[0].cleanup)->role == rules::ArrowRole::Cleanup,
          "find_arrow resolves the cleanup");
    check(analysis.find_arrow(nullptr) == nullptr, "an unknown arrow has no role");
}

void test_unannotated_arrow_that_is_not_setter_only_is_rejected() {
    const auto call = analyze(component(kState + "  const f = () => { go(); };\n"));
    check_eq(describe_all(*call),
             "TESSERA_REACT_BIND_MISSING | 4:3 | const f = () => { go(); }; | handler `f` has no "
             "`// Note: bind::<id>` annotation and its body is not made of setter calls only\n",
             "a call that is not a setter");
    check_eq(call->analyzed.diagnostics[0].suggestion,
             "add `// Note: bind::<id>` on the line before it, or reduce the body to setter calls",
             "the suggestion offers both fixes");
    check(!call->analyzed.components[0].ok, "the component is flagged");
    check_eq(role_text(call->analyzed.components[0]), "Other ", "a rejected arrow has role Other");

    const auto mixed = analyze(component(kState + "  const f = () => { setCount(1); go(); };\n"));
    check(mixed->analyzed.diagnostics.size() == 1 &&
              mixed->analyzed.diagnostics[0].code == "TESSERA_REACT_BIND_MISSING",
          "one non-setter statement is enough");
    const auto value = analyze(component(kState + "  const f = () => count + 1;\n"));
    check(value->analyzed.diagnostics.size() == 1 &&
              value->analyzed.diagnostics[0].code == "TESSERA_REACT_BIND_MISSING",
          "an expression body that is not a setter call");
    const auto empty = analyze(component(kState + "  const f = () => {};\n"));
    check(empty->analyzed.diagnostics.size() == 1 &&
              empty->analyzed.diagnostics[0].code == "TESSERA_REACT_BIND_MISSING",
          "an empty body is not an inline handler");
    const auto not_setter = analyze(component(kState + "  const f = () => v.get();\n"));
    check(not_setter->analyzed.diagnostics.size() == 1,
          "only `.set` of an animated value counts as a setter");
    const auto unknown_setter = analyze(component("  const f = () => setNope(1);\n"));
    check(unknown_setter->analyzed.diagnostics.size() == 1,
          "a call of an undeclared setter is not a setter call");
}

void test_annotated_arrow_may_have_any_body() {
    const auto fixture = analyze(component("  // Note: bind::go\n"
                                           "  const f = (n: int): int => { go(n); return n; };\n"));
    check_eq(describe_all(*fixture), "", "a native handler body is a preview body, return allowed");
    check_eq(role_text(fixture->analyzed.components[0]), "NativeHandler(go) ", "role");
}

void test_effect_return_placement() {
    const auto cleanup =
        analyze(component("  useEffect(() => { go(); return () => { stop(); }; }, []);\n"));
    check_eq(describe_all(*cleanup), "", "return of a cleanup as the last statement is accepted");

    const auto middle =
        analyze(component("  useEffect(() => { return () => { stop(); }; go(); }, []);\n"));
    check_eq(describe_all(*middle),
             "TESSERA_REACT_EFFECT_RETURN | 2:21 | return () => { stop(); } | `return` is only "
             "allowed as the last statement of an effect body, in the form of a cleanup\n",
             "a return that is not last");
    check(!middle->analyzed.components[0].ok, "the component is flagged");

    const auto value = analyze(component("  useEffect(() => { go(); return 1; }, []);\n"));
    check_eq(describe_all(*value),
             "TESSERA_REACT_EFFECT_RETURN | 2:27 | return 1 | the final `return` of an effect "
             "must return a cleanup arrow function\n",
             "the last return must return an arrow");

    const auto bare = analyze(component("  useEffect(() => { go(); return; }, []);\n"));
    check(bare->analyzed.diagnostics.size() == 1 &&
              bare->analyzed.diagnostics[0].code == "TESSERA_REACT_EFFECT_RETURN",
          "a bare return");

    const auto in_cleanup =
        analyze(component("  useEffect(() => { return () => { stop(); return 1; }; }, []);\n"));
    check_eq(describe_all(*in_cleanup),
             "TESSERA_REACT_EFFECT_RETURN | 2:44 | return 1 | `return` is only allowed as the "
             "last statement of an effect body, in the form of a cleanup\n",
             "a return inside the cleanup body is also misplaced");
}

void test_declared_arrow_order_of_reports() {
    const auto fixture = analyze(component(kState + "  const a = () => { one(); };\n"
                                                    "  const b = () => { two(); };\n"));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_BIND_MISSING | 4:3 | const a = () => { one(); }; | handler `a` has "
             "no `// Note: bind::<id>` annotation and its body is not made of setter calls only\n"
             "TESSERA_REACT_BIND_MISSING | 5:3 | const b = () => { two(); }; | handler `b` has "
             "no `// Note: bind::<id>` annotation and its body is not made of setter calls only\n",
             "one report per arrow, in source order");
}

} // namespace

int main() {
    test_roles_of_every_arrow();
    test_unannotated_arrow_that_is_not_setter_only_is_rejected();
    test_annotated_arrow_may_have_any_body();
    test_effect_return_placement();
    test_declared_arrow_order_of_reports();
    return finish("rules arrows tests");
}
