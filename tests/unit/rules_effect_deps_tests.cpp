// SPDX-License-Identifier: MIT

// Effect dependency arrays are exact (docs/specs/component-model-v1.md section 2,
// docs/specs/jsx-subset-v1.md section 7).

#include "rules_test_util.hpp"

namespace {

using namespace rules_test;

const std::string kHooks = "  const [count, setCount] = useState<int>(0);\n"
                           "  const [name, setName] = useState(\"a\");\n"
                           "  const v = useAnimatedValue(0);\n"
                           "  const r = useRef<View>(null);\n";

std::string effect(std::string_view body, std::string_view deps) {
    return kHooks + "  useEffect(() => { " + std::string(body) + " }, " + std::string(deps) +
           ");\n";
}

void test_exact_dependencies_pass() {
    const auto fixture = analyze(component(effect("log(count, v); setName(\"x\");", "[count, v]"),
                                           "<View/>", "{ label }: P"));
    check_eq(describe_all(*fixture), "", "state and animated value read, setter and ref not");
    const auto& analysis = fixture->analyzed.components[0];
    check(analysis.effects.size() == 1, "one effect");
    check(analysis.effects[0].reads == std::vector<std::string>({"count", "v"}),
          "reads are in first-read order");
    check(analysis.effects[0].dependencies == std::vector<std::string>({"count", "v"}),
          "dependencies are kept as written");

    const auto none = analyze(component(effect("go();", "[]")));
    check_eq(describe_all(*none), "", "an effect that reads nothing has no dependencies");
}

void test_order_of_the_array_does_not_matter() {
    const auto fixture = analyze(component(effect("log(count, name);", "[name, count]")));
    check_eq(describe_all(*fixture), "", "the dependency array is a set");
}

void test_missing_dependency() {
    const auto fixture = analyze(component(effect("log(count, name);", "[count]")));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_EFFECT_DEPS | 6:42 | [count] | the effect reads `name` but does not "
             "list it as a dependency\n",
             "reported on the array");
    check_eq(fixture->analyzed.diagnostics[0].suggestion, "add `name` to the dependency array",
             "the suggestion names the dependency");
    check(!fixture->analyzed.components[0].ok, "the component is flagged");

    const auto two = analyze(component(effect("log(name, count);", "[]")));
    check_eq(describe_all(*two),
             "TESSERA_REACT_EFFECT_DEPS | 6:42 | [] | the effect reads `name` but does not list "
             "it as a dependency\n"
             "TESSERA_REACT_EFFECT_DEPS | 6:42 | [] | the effect reads `count` but does not "
             "list it as a dependency\n",
             "missing names are reported in the order they are read");
}

void test_extra_dependency() {
    const auto fixture = analyze(component(effect("log(count);", "[count, name]")));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_EFFECT_DEPS | 6:44 | name | `name` is listed but the effect body does "
             "not read it\n",
             "reported on the extra entry");
    check_eq(fixture->analyzed.diagnostics[0].suggestion, "remove `name` from the dependency array",
             "the suggestion says what to remove");
}

void test_duplicate_dependency() {
    const auto fixture = analyze(component(effect("log(count);", "[count, count]")));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_EFFECT_DEPS | 6:44 | count | `count` is listed twice\n",
             "the second entry is the duplicate");
}

void test_dependencies_must_be_plain_names() {
    const auto expression = analyze(component(effect("log(count);", "[count + 1]")));
    check_eq(describe_all(*expression),
             "TESSERA_REACT_EFFECT_DEPS | 6:37 | count + 1 | an effect dependency must be a plain "
             "name, not an expression\n"
             "TESSERA_REACT_EFFECT_DEPS | 6:36 | [count + 1] | the effect reads `count` but does "
             "not list it as a dependency\n",
             "an expression is rejected, and what it stood for is still missing");

    const auto not_array = analyze(component(effect("go();", "deps")));
    check_eq(describe_all(*not_array),
             "TESSERA_REACT_EFFECT_DEPS | 6:30 | deps | the effect dependencies must be an array "
             "literal of names\n",
             "the second argument must be an array literal");
}

void test_names_that_cannot_be_dependencies() {
    const auto fixture =
        analyze(component("  const [count, setCount] = useState<int>(0);\n"
                          "  const r = useRef<View>(null);\n"
                          "  const twice = count * 2;\n"
                          "  useEffect(() => { go(); }, [setCount, r, twice, nope]);\n"));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_EFFECT_DEPS | 5:31 | setCount | `setCount` is a state setter, so it "
             "cannot be an effect dependency\n"
             "TESSERA_REACT_EFFECT_DEPS | 5:41 | r | `r` is a ref, so it cannot be an effect "
             "dependency\n"
             "TESSERA_REACT_EFFECT_DEPS | 5:44 | twice | `twice` is a local constant, so it "
             "cannot be an effect dependency\n"
             "TESSERA_REACT_EFFECT_DEPS | 5:51 | nope | `nope` is not declared in this "
             "component, so it cannot be an effect dependency\n",
             "setters, refs, locals and unknown names are not dependencies");
}

void test_missing_dependency_array() {
    const auto fixture = analyze(component(kHooks + "  useEffect(() => { go(); });\n"));
    check_eq(describe_all(*fixture),
             "TESSERA_REACT_EFFECT_DEPS | 6:3 | useEffect(() => { go(); }); | `useEffect` needs "
             "a dependency array (V1 dependencies are exact)\n",
             "the array is required");
}

void test_locals_are_expanded() {
    const auto fixture =
        analyze(component(kHooks + "  const twice = count * 2;\n"
                                   "  const label = twice + 1;\n"
                                   "  useEffect(() => { log(label); }, [count]);\n"));
    check_eq(describe_all(*fixture), "", "a read of a local requires what the local reads");
    check(fixture->analyzed.components[0].effects[0].reads == std::vector<std::string>({"count"}),
          "transitively through two locals");

    const auto missing = analyze(component(kHooks + "  const twice = count * 2;\n"
                                                    "  useEffect(() => { log(twice); }, []);\n"));
    check_eq(describe_all(*missing),
             "TESSERA_REACT_EFFECT_DEPS | 7:36 | [] | the effect reads `count` but does not list "
             "it as a dependency\n",
             "the dependency is reported under the name of the state, not the local");
}

void test_props() {
    const auto destructured = analyze(
        component("  useEffect(() => { log(label); }, [label]);\n", "<View/>", "{ label }: P"));
    check_eq(describe_all(*destructured), "", "a destructured prop is a dependency by name");

    const auto missing =
        analyze(component("  useEffect(() => { log(label); }, []);\n", "<View/>", "{ label }: P"));
    check(missing->analyzed.diagnostics.size() == 1 &&
              missing->analyzed.diagnostics[0].code == "TESSERA_REACT_EFFECT_DEPS",
          "a missing prop dependency");

    const auto member = analyze(
        component("  useEffect(() => { log(props.label); }, []);\n", "<View/>", "props: P"));
    check_eq(describe_all(*member),
             "TESSERA_REACT_EFFECT_DEPS | 2:42 | [] | the effect reads `props.label` but does not "
             "list it as a dependency\n",
             "a member of the props parameter cannot be listed");
    check_eq(member->analyzed.diagnostics[0].suggestion,
             "destructure the props parameter so the prop has a name to list",
             "the suggestion explains the way out");
}

void test_shadowing_and_callees() {
    const auto fixture =
        analyze(component(kHooks + "  useEffect(() => { go((count) => count); }, []);\n"));
    check(fixture->parsed.ok(), "shadowing fixture parses");
    check_eq(describe_all(*fixture), "",
             "an arrow parameter named like state is not a read of the state");

    const auto callee =
        analyze(component(kHooks + "  useEffect(() => { setCount(1); v.set(2); }, []);\n"));
    check_eq(describe_all(*callee), "", "setter calls and `.set` do not make dependencies");
}

} // namespace

int main() {
    test_exact_dependencies_pass();
    test_order_of_the_array_does_not_matter();
    test_missing_dependency();
    test_extra_dependency();
    test_duplicate_dependency();
    test_dependencies_must_be_plain_names();
    test_names_that_cannot_be_dependencies();
    test_missing_dependency_array();
    test_locals_are_expanded();
    test_props();
    test_shadowing_and_callees();
    return finish("rules effect deps tests");
}
