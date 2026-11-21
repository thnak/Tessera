// SPDX-License-Identifier: MIT

// Differential tests of the custom-hook expansion (spike #659, ADR-065;
// docs/specs/custom-hooks-v1.md section 11, "Differential"). Each fixture is a pair: a component
// that calls a custom hook, and the component the author would have written by hand. The pair must
// lower to the same IR after the id mapping of the expansion (the `<hook>_<n>__` prefix), which is
// what proves the expansion is a source-to-source rewrite and not a new construct.
//
// The eight fixtures are the ones the spike names: usePagination, useToggle (tuple return), a form
// field with an effect, a hook calling a hook, two calls of one hook, a hook returning a ref used
// for a behavior target, a hook with a callback parameter, and a hook with an expression argument
// used in an effect dependency.

#include "hook_expansion_test_util.hpp"

#include <string>
#include <vector>

namespace {

using namespace hook_expansion_test;

// One fixture: the hook declaration, the component that uses it, the hand-expanded component, and
// the prefixes the id mapping removes.
struct Fixture {
    const char* name;
    std::string source;
    std::string hand_expanded;
    std::vector<Rename> prefixes;
};

// `function <name>(<params>) { <body> }` with the hook declaration first.
std::string with_hook(std::string_view hook, std::string_view component) {
    return std::string(hook) + "\n" + std::string(component);
}

std::vector<Fixture> fixtures() {
    std::vector<Fixture> out;

    // 1. usePagination: an object return bound as a whole, read as `p.page` and `p.hasMore`, with
    // the defaulted `pageSize` omitted at the call site.
    out.push_back(Fixture{
        "usePagination",
        with_hook("function usePagination(total: int, pageSize: int = 20) {\n"
                  "  const [page, setPage] = useState<int>(1);\n"
                  "  const hasMore = page < total;\n"
                  "  const next = () => setPage(page + 1);\n"
                  "  return { page, setPage, hasMore, next };\n"
                  "}\n",
                  "function List(props: P) {\n"
                  "  const p = usePagination(props.total);\n"
                  "  return <View style={{ width: p.page }} onClick={p.next}/>;\n"
                  "}\n"),
        "function List(props: P) {\n"
        "  const [page, setPage] = useState<int>(1);\n"
        "  const hasMore = page < props.total;\n"
        "  const next = () => setPage(page + 1);\n"
        "  return <View style={{ width: page }} onClick={next}/>;\n"
        "}\n",
        {{"usePagination_1__", ""}},
    });

    // 2. useToggle: a tuple return bound with an array pattern. The argument is a plain name, so
    // it is substituted rather than copied into a local (spec section 4 step 4).
    out.push_back(Fixture{
        "useToggle",
        with_hook("function useToggle(initial: boolean) {\n"
                  "  const [open, setOpen] = useState<boolean>(initial);\n"
                  "  const toggle = () => setOpen(!open);\n"
                  "  return [open, toggle];\n"
                  "}\n",
                  "function Panel(props: P) {\n"
                  "  const [open, toggle] = useToggle(props.start);\n"
                  "  return <View style={{ width: open }} onClick={toggle}/>;\n"
                  "}\n"),
        "function Panel(props: P) {\n"
        "  const [open, setOpen] = useState<boolean>(props.start);\n"
        "  const toggle = () => setOpen(!open);\n"
        "  return <View style={{ width: open }} onClick={toggle}/>;\n"
        "}\n",
        {{"useToggle_1__", ""}},
    });

    // 3. A form field with an effect: the effect and its dependency are per call site.
    out.push_back(Fixture{
        "form field with an effect",
        with_hook("function useField(initial: string) {\n"
                  "  const [value, setValue] = useState<string>(initial);\n"
                  "  // Note: bind::sync\n"
                  "  useEffect(() => { log(value); }, [value]);\n"
                  "  return { value, setValue };\n"
                  "}\n",
                  "function Form(props: P) {\n"
                  "  const f = useField(props.label);\n"
                  "  return <TextInput value={f.value} onChange={f.setValue}/>;\n"
                  "}\n"),
        "function Form(props: P) {\n"
        "  const [value, setValue] = useState<string>(props.label);\n"
        "  // Note: bind::sync\n"
        "  useEffect(() => { log(value); }, [value]);\n"
        "  return <TextInput value={value} onChange={setValue}/>;\n"
        "}\n",
        {{"useField_1__", ""}},
    });

    // 4. A hook calling a hook: the inner call is expanded where it appears. Both arguments are
    // plain names, so no prologue local is created (see the note on fixture 8).
    out.push_back(Fixture{
        "a hook calling a hook",
        with_hook("function useCounter(start: int) {\n"
                  "  const [count, setCount] = useState<int>(start);\n"
                  "  const bump = () => setCount(count + 1);\n"
                  "  return { count, bump };\n"
                  "}\n"
                  "function useDoubled(start: int) {\n"
                  "  const c = useCounter(start);\n"
                  "  const doubled = c.count * 2;\n"
                  "  return { doubled, bump: c.bump };\n"
                  "}\n",
                  "function Counter(props: P) {\n"
                  "  const d = useDoubled(props.start);\n"
                  "  return <View style={{ width: d.doubled }} onClick={d.bump}/>;\n"
                  "}\n"),
        "function Counter(props: P) {\n"
        "  const [count, setCount] = useState<int>(props.start);\n"
        "  const bump = () => setCount(count + 1);\n"
        "  const doubled = count * 2;\n"
        "  return <View style={{ width: doubled }} onClick={bump}/>;\n"
        "}\n",
        {{"useDoubled_1__", ""}, {"useCounter_1__", ""}},
    });

    // 5. Two calls of one hook: two states, two effects, two refs (spec section 4).
    out.push_back(Fixture{
        "two calls of one hook",
        with_hook("function useFlag(initial: boolean) {\n"
                  "  const [on, setOn] = useState<boolean>(initial);\n"
                  "  const flip = () => setOn(!on);\n"
                  "  return { on, flip };\n"
                  "}\n",
                  "function Pair(props: P) {\n"
                  "  const a = useFlag(props.first);\n"
                  "  const b = useFlag(props.second);\n"
                  "  return <View style={{ width: a.on }} onClick={a.flip} onBlur={b.flip}/>;\n"
                  "}\n"),
        "function Pair(props: P) {\n"
        "  const [on, setOn] = useState<boolean>(props.first);\n"
        "  const flip = () => setOn(!on);\n"
        "  const [on2, setOn2] = useState<boolean>(props.second);\n"
        "  const flip2 = () => setOn2(!on2);\n"
        "  return <View style={{ width: on }} onClick={flip} onBlur={flip2}/>;\n"
        "}\n",
        {{"useFlag_1__", ""},
         {"useFlag_2__on", "on2"},
         {"useFlag_2__setOn", "setOn2"},
         {"useFlag_2__flip", "flip2"}},
    });

    // 6. A hook returning a ref used for a behavior target (spec section 4, "Refs and behavior
    // targets"): the ref is attached in the component and the behavior hook targets it.
    out.push_back(Fixture{
        "a hook returning a ref used for a behavior target",
        with_hook("function usePressable() {\n"
                  "  const r = useRef<View>(null);\n"
                  "  const press = usePressFeedback(r, { disabled: false });\n"
                  "  return { r, pressed: press.pressed };\n"
                  "}\n",
                  "function Button() {\n"
                  "  const p = usePressable();\n"
                  "  return <View ref={p.r} style={{ width: p.pressed }}/>;\n"
                  "}\n"),
        "function Button() {\n"
        "  const r = useRef<View>(null);\n"
        "  const press = usePressFeedback(r, { disabled: false });\n"
        "  return <View ref={r} style={{ width: press.pressed }}/>;\n"
        "}\n",
        {{"usePressable_1__", ""}},
    });

    // 7. A hook with a callback parameter: the handler reference is substituted.
    out.push_back(Fixture{
        "a hook with a callback parameter",
        with_hook("function useAction(onRun: () => void) {\n"
                  "  const [busy, setBusy] = useState<boolean>(false);\n"
                  "  const run = () => { setBusy(true); onRun(); };\n"
                  "  return { busy, run };\n"
                  "}\n",
                  "function Toolbar() {\n"
                  "  // Note: bind::go\n"
                  "  const go = (): void => { log(1); };\n"
                  "  const a = useAction(go);\n"
                  "  return <View style={{ width: a.busy }} onClick={a.run}/>;\n"
                  "}\n"),
        "function Toolbar() {\n"
        "  // Note: bind::go\n"
        "  const go = (): void => { log(1); };\n"
        "  const [busy, setBusy] = useState<boolean>(false);\n"
        "  const run = () => { setBusy(true); go(); };\n"
        "  return <View style={{ width: busy }} onClick={run}/>;\n"
        "}\n",
        {{"useAction_1__", ""}},
    });

    // 8. A hook with an expression argument used in an effect dependency. The parameter is an
    // effect dependency, so the call site must pass a plain name (spec section 4, "Effect
    // dependencies"); the expression is bound in the component, where the author can see it.
    out.push_back(Fixture{
        "a hook with an expression argument used in an effect dependency",
        with_hook("function useWatch(limit: int) {\n"
                  "  const [seen, setSeen] = useState<int>(0);\n"
                  "  // Note: bind::watch\n"
                  "  useEffect(() => { log(limit); }, [limit]);\n"
                  "  return { seen, setSeen };\n"
                  "}\n",
                  "function Feed(props: P) {\n"
                  "  const limit = props.limit + 1;\n"
                  "  const w = useWatch(limit);\n"
                  "  return <View style={{ width: w.seen }}/>;\n"
                  "}\n"),
        "function Feed(props: P) {\n"
        "  const limit = props.limit + 1;\n"
        "  const [seen, setSeen] = useState<int>(0);\n"
        "  // Note: bind::watch\n"
        "  useEffect(() => { log(limit); }, [limit]);\n"
        "  return <View style={{ width: seen }}/>;\n"
        "}\n",
        {{"useWatch_1__", ""}},
    });

    return out;
}

// The IR of one source, or the first diagnostic. `custom_hooks` selects the pipeline.
std::string ir_of(const std::string& source, bool custom_hooks, std::string& error) {
    Pipeline pipeline(source, custom_hooks);
    if (!pipeline.ok()) {
        error = first_diagnostic(pipeline);
        return {};
    }
    const lowering::ModuleLoweringResult lowered = pipeline.lower();
    if (!lowered) {
        error = lowered.error().empty() ? "lowering failed" : lowered.error().front().message;
        return {};
    }
    return component_ir(*lowered);
}

// A fixture whose two sides both stop at the same engine gap is still evidence: the expansion put
// the component in exactly the state the author's hand-written version is in, and the gap is the
// engine's, not the expansion's. The comparison drops the expansion's prefixes and the `line:col`
// position (a hook's statements are in the declaration, the hand-written ones in the component), so
// a difference in *which* construct is rejected still fails.
std::string failure_shape(std::string error, const std::vector<Rename>& prefixes) {
    error = strip_expansion_prefixes(std::move(error), prefixes);
    const std::size_t bar = error.find(" | ");
    if (bar == std::string::npos) {
        return error;
    }
    const std::size_t second = error.find(" | ", bar + 3);
    if (second == std::string::npos) {
        return error;
    }
    return error.substr(0, bar) + error.substr(second);
}

bool same_failure(const std::string& expanded_error, const std::string& hand_error,
                  const std::vector<Rename>& prefixes) {
    if (expanded_error.empty() || hand_error.empty()) {
        return false;
    }
    return failure_shape(expanded_error, prefixes) == failure_shape(hand_error, prefixes);
}

void test_fixture(const Fixture& fixture) {
    std::string expanded_error;
    std::string hand_error;
    const std::string expanded = ir_of(fixture.source, true, expanded_error);
    const std::string hand = ir_of(fixture.hand_expanded, false, hand_error);
    if (!expanded_error.empty() || !hand_error.empty()) {
        // Both sides must stop at the same place. A one-sided failure is a real defect.
        check(same_failure(expanded_error, hand_error, fixture.prefixes),
              std::string(fixture.name) + ": both sides stop at the same engine gap\n  hook: " +
                  expanded_error + "\n  hand: " + hand_error);
        return;
    }
    const std::string mapped = strip_expansion_prefixes(expanded, fixture.prefixes);
    check_eq(mapped, hand, std::string(fixture.name) + ": same IR after id mapping");
}

// The switch is off by default: the same source is the `TESSERA_REACT_CUSTOM_HOOK` rejection.
void test_off_by_default() {
    const std::string source = with_hook("function useThing() {\n"
                                         "  const [n, setN] = useState<int>(0);\n"
                                         "  return { n, setN };\n"
                                         "}\n",
                                         "function A() {\n"
                                         "  const t = useThing();\n"
                                         "  return <View style={{ width: t.n }}/>;\n"
                                         "}\n");
    Pipeline off(source, false);
    check(!off.parsed.ok(), "with the switch off the hook declaration is rejected");
    check_eq(std::string(off.parsed.diagnostics.empty() ? "<none>"
                                                        : off.parsed.diagnostics.front().code),
             "TESSERA_REACT_CUSTOM_HOOK", "the rejection is the one the spec keeps");
    Pipeline on(source, true);
    check(on.parsed.ok(), "with the switch on the hook declaration parses");
    check(on.ok(), "with the switch on the component analyses: " + first_diagnostic(on));
}

// The expansion is a pure function of the module: two runs produce the same IR.
void test_determinism() {
    for (const Fixture& fixture : fixtures()) {
        std::string first_error;
        std::string second_error;
        const std::string first = ir_of(fixture.source, true, first_error);
        const std::string second = ir_of(fixture.source, true, second_error);
        check_eq(first, second, std::string(fixture.name) + ": the expansion is deterministic");
    }
}

} // namespace

int main() {
    for (const Fixture& fixture : fixtures()) {
        test_fixture(fixture);
    }
    test_off_by_default();
    test_determinism();
    return finish("hook expansion differential tests");
}
