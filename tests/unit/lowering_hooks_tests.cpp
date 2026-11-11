// SPDX-License-Identifier: MIT

// Slice 6: hooks and bindings lower to IR component data (docs/design/jsx-ir-lowering.md slice 6,
// docs/specs/ir-v1.md sections 2, 2.1 and 6).

#include "lowering_hooks_test_util.hpp"

namespace {

using namespace lowering_hooks_test;

void test_state_forms() {
    const auto fixture =
        parse(component("  const [count, setCount] = useState<int>(props.initial);\n"
                        "  const [ratio, setRatio] = useState(0.5);\n"
                        "  const [name, setName] = useState(\"a\");\n"
                        "  const [flag, setFlag] = useState(false);\n"
                        "  const [maybe, setMaybe] = useState<string | null>(null);\n"
                        "  const [rows, setRows] = useState<Row[]>(props.rows);\n",
                        "<View/>", "props: P"));
    const auto result = lower(*fixture);
    check(result.has_value(), "the state hooks lower: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const lowering::LoweredHooks& hooks = *result;
    check_eq(hooks.component_id, "comp::A", "the component id is comp::<Name>");
    check(hooks.state.size() == 6, "one state entry per useState");
    check(hooks.setter_index.size() == 6, "one setter per useState");
    if (hooks.state.size() != 6) {
        return;
    }
    check_eq(hooks.state[0].id, "state::count", "the state id");
    check_eq(hooks.state[0].name, "count", "the state name");
    check_eq(hooks.state[0].type, "int", "the written type argument becomes the IR type");
    check_eq(expr_text(hooks.state[0].initial.get()), "{\"kind\":\"ref\",\"id\":\"prop::initial\"}",
             "the initial value is a lowered expression");
    check_eq(hooks.state[1].type, "float", "a fraction literal infers float");
    check_eq(expr_text(hooks.state[1].initial.get()), "{\"kind\":\"literal\",\"value\":0.5}",
             "the float initial is a float literal");
    check_eq(hooks.state[2].type, "string", "a string literal infers string");
    check_eq(hooks.state[3].type, "bool", "a boolean literal infers bool");
    check_eq(hooks.state[4].type, "optional<string>", "T | null becomes optional<T>");
    check_eq(expr_text(hooks.state[4].initial.get()), "{\"kind\":\"literal\",\"value\":null}",
             "the null initial lowers to a null literal");
    check_eq(hooks.state[5].type, "list<Row>", "T[] becomes list<T>");
    check_eq(expr_text(hooks.state[5].initial.get()), "{\"kind\":\"ref\",\"id\":\"prop::rows\"}",
             "props.x resolves to a prop reference");
    // `const char*` rather than `const std::string&`: the braced list is an initializer_list of
    // string literals, and GCC's -Wrange-loop-construct (CI builds with -Werror) rejects binding a
    // reference to the temporary std::string that would be constructed per element.
    for (const char* setter :
         {"setCount", "setRatio", "setName", "setFlag", "setMaybe", "setRows"}) {
        check(hooks.find_setter(setter) != nullptr, std::string("setter ") + setter + " indexed");
    }
    const lowering::NamedSetter* setter = hooks.find_setter("setCount");
    check(setter != nullptr && setter->state_id == "state::count", "a setter maps to its state id");
    check(hooks.find_setter("missing") == nullptr, "an unknown setter is not indexed");
}

void test_array_list_initials() {
    const auto fixture =
        parse(component("  const [names, setNames] = useState<string[]>([]);\n"
                        "  const [ids, setIds] = useState<int[]>([1, 2, 3]);\n"
                        "  const [flags, setFlags] = useState<boolean[]>([true, false]);\n"
                        "  const [ratios, setRatios] = useState<float[]>([0.5, 1]);\n"));
    const auto result = lower(*fixture);
    check(result.has_value(), "array list initials lower: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const lowering::LoweredHooks& hooks = *result;
    check(hooks.state.size() == 4, "one state entry per list useState");
    if (hooks.state.size() != 4) {
        return;
    }
    check_eq(hooks.state[0].type, "list<string>", "`string[]` becomes `list<string>`");
    check_eq(expr_text(hooks.state[0].initial.get()), "{\"kind\":\"array\",\"items\":[]}",
             "an empty list initial is an empty IR array");
    check_eq(hooks.state[1].type, "list<int>", "`int[]` becomes `list<int>`");
    check(expr_text(hooks.state[1].initial.get()).find("\"kind\":\"array\"") != std::string::npos,
          "a literal-filled list initial is an IR array");
    check(expr_text(hooks.state[1].initial.get()).find("\"value\":3") != std::string::npos,
          "the literal elements are lowered in order");
    check_eq(hooks.state[2].type, "list<bool>", "`boolean[]` becomes `list<bool>`");
    check_eq(hooks.state[3].type, "list<float>", "`float[]` becomes `list<float>`");
    for (const char* setter : {"setNames", "setIds", "setFlags", "setRatios"}) {
        check(hooks.find_setter(setter) != nullptr, std::string("setter ") + setter + " indexed");
    }
}

void test_refs_and_animated_values() {
    const auto fixture = parse(component("  const r = useRef<View>(null);\n"
                                         "  const v = useAnimatedValue(1.0);\n"));
    auto result = lower(*fixture);
    check(result.has_value(), "the ref and animated value lower");
    if (!result) {
        return;
    }
    lowering::LoweredHooks& hooks = *result;
    check(hooks.refs.size() == 1, "one ref entry");
    check(hooks.animated_values.size() == 1, "one animated value entry");
    if (hooks.refs.size() != 1 || hooks.animated_values.size() != 1) {
        return;
    }
    check_eq(hooks.refs[0].id, "ref::r", "the ref id");
    check_eq(hooks.refs[0].name, "r", "the ref name");
    check_eq(hooks.refs[0].target, "", "the node target stays for the node lowering of slice 7");
    check(hooks.find_ref("r") != nullptr, "the ref is indexed by name");
    check_eq(hooks.animated_values[0].id, "anim::v", "the animated value id");
    check_eq(hooks.animated_values[0].name, "v", "the animated value name");
    check_eq(hooks.animated_values[0].type, "float", "an animated value is a float");
    check_eq(expr_text(hooks.animated_values[0].initial.get()),
             "{\"kind\":\"literal\",\"value\":1.0}", "a fraction initial stays a float literal");
    check(hooks.find_animated("v") != nullptr, "the animated value is indexed by name");
    check(hooks.set_ref_target("r", "node::2"), "the ref target can be filled by slice 7");
    check_eq(hooks.refs[0].target, "node::2", "the ref target is stored");
    check(!hooks.set_ref_target("missing", "node::1"), "an unknown ref is rejected");
}

void test_effects_cleanup_and_native_bindings() {
    const auto fixture = parse(component(
        "  const [count, setCount] = useState<int>(0);\n"
        "  const v = useAnimatedValue(1.0);\n"
        "  // Note: bind::on_click\n"
        "  const clicked = (): void => { setCount(1); };\n"
        "  // Note: bind::sync\n"
        "  useEffect(() => { log(initial); log(count); log(v); return () => { stop(); }; }, "
        "[initial, count, v]);\n",
        "<View/>", "{ initial = 0 }: P"));
    const auto result = lower(*fixture);
    check(result.has_value(),
          "the effect and the native handler lower: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    const lowering::LoweredHooks& hooks = *result;
    check(hooks.effects.size() == 1, "one effect entry");
    if (hooks.effects.size() != 1) {
        return;
    }
    const ir::Effect& effect = hooks.effects[0];
    check_eq(effect.id, "effect::sync", "the effect id comes from the bind id");
    check_eq(effect.handler, "bind::sync", "the effect handler is its binding");
    check_eq(effect.cleanup, "bind::sync_cleanup", "the cleanup is its own binding id");
    check(effect.deps == std::vector<std::string>({"prop::initial", "state::count", "anim::v"}),
          "dependencies are mapped to prop, state and animated ids in written order");
    check_eq(span_text(effect.source), "ui/Counter.tsx:7:3",
             "the effect span is the useEffect statement");
    check(hooks.bindings.size() == 3, "on_click, sync and sync_cleanup bindings");
    if (hooks.bindings.size() != 3) {
        return;
    }
    // docs/design/jsx-ir-lowering.md section 5: bindings are sorted by id.
    check_eq(hooks.bindings[0].id, "bind::on_click", "bindings are sorted by id (1)");
    check_eq(hooks.bindings[1].id, "bind::sync", "bindings are sorted by id (2)");
    check_eq(hooks.bindings[2].id, "bind::sync_cleanup", "bindings are sorted by id (3)");
    check(hooks.bindings[0].kind == ir::BindingKind::Event, "a native handler is an event binding");
    check_eq(hooks.bindings[0].region, "on_click", "the region is the bind id");
    check_eq(hooks.bindings[0].component, "comp::A", "the binding names its component");
    check_eq(span_text(hooks.bindings[0].source), "ui/Counter.tsx:4:3",
             "a binding's span is the annotation comment");
    check_eq(span_text(hooks.bindings[1].source), "ui/Counter.tsx:6:3",
             "an effect binding's span is its annotation comment");
    check_eq(span_text(hooks.bindings[2].source), "ui/Counter.tsx:7:62",
             "the cleanup binding's span is the cleanup arrow");
    check(hooks.bindings[0].params.empty(), "the parameterless handler has no params");
    check_eq(hooks.bindings[0].return_type, "", "`: void` is an absent return type");
    check(hooks.bindings[1].kind == ir::BindingKind::Effect, "an effect binding has kind effect");
    check(hooks.bindings[2].kind == ir::BindingKind::Effect, "the cleanup is an effect binding");
    const lowering::HandlerRef* native = hooks.find_handler("clicked");
    check(native != nullptr && native->kind == lowering::HandlerRefKind::Binding &&
              native->id == "bind::on_click",
          "a declared native handler resolves to its binding");
}

void test_inline_setter_handlers() {
    const auto fixture = parse(component(
        "  const [count, setCount] = useState<int>(0);\n"
        "  const [label, setLabel] = useState(\"idle\");\n"
        "  const v = useAnimatedValue(1.0);\n"
        "  const reset = () => { setCount(0); setLabel(\"idle\"); v.set(1.0, { animate: false }); "
        "};\n"
        "  const bump = () => { setCount(count + 1); v.set(0.5); };\n"));
    auto result = lower(*fixture);
    check(result.has_value(), "the inline handlers lower: " + describe_errors(*fixture, result));
    if (!result) {
        return;
    }
    lowering::LoweredHooks& hooks = *result;
    check(hooks.handlers.size() == 2, "one handler record per declared setter-call arrow");
    check(hooks.bindings.empty(), "an inline handler creates no protected region");
    if (hooks.handlers.size() != 2) {
        return;
    }
    check_eq(hooks.handlers[0].id, "handler::1", "handlers are numbered from 1 in source order");
    check_eq(hooks.handlers[1].id, "handler::2", "the second handler");
    check(hooks.handlers[0].params.empty(), "a parameterless inline handler has no params");
    check_eq(span_text(hooks.handlers[0].source), "ui/Counter.tsx:5:17",
             "a handler record spans its arrow");
    check(hooks.handlers[0].actions.size() == 3, "three actions");
    check(hooks.handlers[1].actions.size() == 2, "two actions");
    if (hooks.handlers[0].actions.size() != 3 || hooks.handlers[1].actions.size() != 2) {
        return;
    }
    check_eq(action_text(hooks.handlers[0].actions[0]),
             "set state::count = "
             "{\"kind\":\"literal\",\"value\":0}",
             "a setter call becomes a set action");
    check_eq(action_text(hooks.handlers[0].actions[2]),
             "animSet anim::v = {\"kind\":\"literal\",\"value\":1.0} animate=false",
             "`.set(value, { animate: false })` becomes an animSet action");
    check_eq(action_text(hooks.handlers[1].actions[0]),
             "set state::count = {\"kind\":\"binary\",\"op\":\"+\",\"left\":{\"kind\":\"ref\","
             "\"id\":\"state::count\"},\"right\":{\"kind\":\"literal\",\"value\":1}}",
             "setter arguments are lowered expressions");
    check_eq(action_text(hooks.handlers[1].actions[1]),
             "animSet anim::v = {\"kind\":\"literal\",\"value\":0.5} animate",
             "`.set(value)` animates by default");
    check(hooks.next_handler_number == 3, "numbering continues after the declared handlers");
    check(hooks.find_handler("reset") != nullptr &&
              hooks.find_handler("reset")->kind == lowering::HandlerRefKind::InlineHandler &&
              hooks.find_handler("reset")->id == "handler::1",
          "a declared inline handler resolves to its record");
    const std::string added =
        hooks.add_handler({}, {}, ir::SourceSpan{"ui/Counter.tsx", 0, 0, 1, 1, 1, 1});
    check_eq(added, "handler::3", "slice 7 can append a node-level handler");
    check(hooks.next_handler_number == 4, "appending advances the numbering");
    check(hooks.handlers.size() == 3, "the appended record is stored");
}

void test_lower_handler_actions_for_event_arrows() {
    const auto fixture = parse(component("  const [label, setLabel] = useState(\"idle\");\n",
                                         "<View onChange={(e) => { setLabel(e); }} />"));
    const rules::ComponentAnalysis* analysis = analysis_of(*fixture);
    check(analysis != nullptr && analysis->item != nullptr, "the fixture has one component");
    if (analysis == nullptr || analysis->item == nullptr || !analysis->item->returned ||
        analysis->item->returned->attributes.size() != 1 ||
        !analysis->item->returned->attributes[0].expr) {
        check(false, "the fixture has one event attribute");
        return;
    }
    const lowering::HookLoweringContext context{*analysis->item, *analysis, fixture->map,
                                                "ui/Counter.tsx"};
    const jsx::Expr& arrow = *analysis->item->returned->attributes[0].expr;
    const lowering::HandlerActionsResult actions = lowering::lower_handler_actions(arrow, context);
    check(actions.has_value(), "an event arrow lowers through the public action seam");
    if (!actions) {
        return;
    }
    check(actions->size() == 1, "one action");
    if (actions->size() == 1) {
        check_eq(action_text(actions->front()),
                 "set state::label = {\"kind\":\"ref\",\"id\":\"e\"}",
                 "a handler parameter resolves to an action local");
    }
}

// jsx-subset-v1.md section 9.3: the TSX body of a native handler is its preview body.
void test_native_preview_bodies() {
    const auto fixture = parse(
        component("  const [count, setCount] = useState<int>(0);\n"
                  "  // Note: bind::add\n"
                  "  const add = (by: int): int => { setCount(count + by); return count + by; };\n"
                  "  // Note: bind::empty\n"
                  "  const nothing = (): void => {};\n"
                  "  // Note: bind::other\n"
                  "  const other = (): void => { log(count); };\n"));
    const auto result = lower(*fixture);
    check(result.has_value(), "the native handlers lower: " + describe_errors(*fixture, result));
    if (!result || result->bindings.size() != 3) {
        check(false, "three bindings");
        return;
    }
    const ir::Binding& add = result->bindings[0];
    check(add.id == "bind::add" && add.preview.has_value() && add.preview->actions.size() == 1,
          "a setter-call body is the preview actions");
    if (add.preview && add.preview->actions.size() == 1) {
        check_eq(add.preview->actions[0].target, "state::count", "the preview action target");
        check_eq(expr_text(add.preview->actions[0].value.get()),
                 "{\"kind\":\"binary\",\"op\":\"+\",\"left\":{\"kind\":\"ref\","
                 "\"id\":\"state::count\"},\"right\":{\"kind\":\"ref\",\"id\":\"by\"}}",
                 "the preview value reads the parameter by name");
        check(add.preview->return_value != nullptr, "a final return is the preview return value");
    }
    check(!result->bindings[1].preview.has_value(), "an empty body has no preview");
    check(!result->bindings[2].preview.has_value(),
          "a body outside the preview rules has no preview and is not an error");
}

void test_empty_component() {
    const auto fixture = parse(component(""));
    const auto result = lower(*fixture);
    check(result.has_value(), "a component without hooks lowers to empty data");
    if (!result) {
        return;
    }
    const lowering::LoweredHooks& hooks = *result;
    check_eq(hooks.component_id, "comp::A", "the component id");
    check(hooks.state.empty() && hooks.handlers.empty() && hooks.effects.empty() &&
              hooks.refs.empty() && hooks.animated_values.empty() && hooks.bindings.empty(),
          "no hook or binding data");
    check_eq(describe(hooks), "component comp::A\nnext handler 1\n", "the full description");
}

} // namespace

int main() {
    test_state_forms();
    test_array_list_initials();
    test_refs_and_animated_values();
    test_effects_cleanup_and_native_bindings();
    test_inline_setter_handlers();
    test_lower_handler_actions_for_event_arrows();
    test_native_preview_bodies();
    test_empty_component();
    return finish("lowering hooks tests");
}
