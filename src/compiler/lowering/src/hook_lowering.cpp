// SPDX-License-Identifier: MIT

// Slice 6 entry point: one analysed component's hook declarations and `bind::` bindings become the
// IR component data of docs/specs/ir-v1.md sections 2, 2.1 and 6. JSX nodes are slice 7 and are not
// read here (the returned element is ignored).

#include "hook_lowering_internal.hpp"
#include "record_internal.hpp"
#include "tessera/compiler/lowering/source_span.hpp"
#include "tessera/compiler/rules/analyze.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tessera::compiler::lowering {
namespace {

using internal::HookState;

std::string prefixed(std::string_view prefix, const std::string& name) {
    return std::string(prefix) + name;
}

bool is_list_type(std::string_view type) {
    return type.size() > 6 && type.rfind("list<", 0) == 0 && type.back() == '>';
}

std::string_view list_element(std::string_view type) {
    return type.substr(5, type.size() - 6);
}

// The element types whose literals the IR can express directly (docs/specs/ir-v1.md section 4).
// Record element types need the type table (slice 4) and are rejected for a non-empty initial.
bool printable_element_type(std::string_view type) {
    return type == "bool" || type == "int" || type == "float" || type == "string" ||
           type == "color" || type == "length";
}

bool literal_matches_element(std::string_view type, const ir::Expr& value) {
    if (value.kind != ir::ExprKind::Literal) {
        return false;
    }
    if (type == "bool") {
        return std::holds_alternative<bool>(value.literal);
    }
    if (type == "int") {
        return std::holds_alternative<std::int64_t>(value.literal);
    }
    if (type == "float" || type == "length") {
        return std::holds_alternative<double>(value.literal) ||
               std::holds_alternative<std::int64_t>(value.literal);
    }
    if (type == "string" || type == "color") {
        return std::holds_alternative<std::string>(value.literal);
    }
    return false;
}

// A list-typed `useState` initial: an array literal with literal elements (spec 8.4), or a
// reference (a prop/list name, the only other list-valued IR expression). Anything else has no
// list-valued IR form, so it is rejected here instead of reaching codegen.
bool validate_list_initial(HookState& state, std::string_view type, const ir::Expr& initial,
                           jsx::Span span) {
    const std::string_view element = list_element(type);
    if (initial.kind == ir::ExprKind::Ref) {
        return true;
    }
    if (initial.kind != ir::ExprKind::Array) {
        internal::fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                            "a `list<" + std::string(element) +
                                ">` state initial must be an array literal or a list reference",
                            span, "write `useState<" + std::string(element) + "[]>([])`");
        return false;
    }
    if (!initial.operands.empty() && !printable_element_type(element)) {
        internal::fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                            "the element type `" + std::string(element) +
                                "` of a filled array initial needs the type table (slice 4)",
                            span, "use an empty `[]` initial or a primitive element type");
        return false;
    }
    for (const ir::ExprPtr& item : initial.operands) {
        if (!item) {
            internal::fail_hook(state, "TESSERA_IR_MALFORMED",
                                "an array initial has a missing element", span);
            return false;
        }
        if (!literal_matches_element(element, *item)) {
            internal::fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                                "array initial elements must be "
                                "`" +
                                    std::string(element) +
                                    "` literals; a dynamic element needs a list-valued IR "
                                    "expression (slice 4)",
                                span, "use `[]` or a list of literals");
            return false;
        }
    }
    return true;
}

void lower_state(HookState& state, std::size_t index, LoweredHooks& out,
                 const ComponentNames& names) {
    const jsx::Statement& statement = state.context.component.body[index];
    const jsx::Expr& call = *statement.init;
    if (statement.kind != jsx::StatementKind::Const || !statement.array_pattern ||
        statement.names.size() != 2 || call.operands.size() != 2 || !call.operands[1]) {
        internal::fail_hook(state, "TESSERA_IR_MALFORMED",
                            "`useState` must be `const [x, setX] = useState<T>(init);`",
                            statement.span);
        return;
    }
    std::string type;
    if (!call.type_args.empty()) {
        if (call.type_args.size() != 1 || !call.type_args[0]) {
            internal::fail_hook(state, "TESSERA_IR_MALFORMED",
                                "`useState` takes at most one type argument", statement.span);
            return;
        }
        const std::optional<std::string> written = internal::lower_written_type(*call.type_args[0]);
        if (!written) {
            internal::fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                                "the state type has no IR form in v1", call.type_args[0]->span,
                                "write an IR type, for example `int`, `float`, `string` or `T[]`");
            return;
        }
        type = *written;
    } else {
        const std::optional<std::string> inferred =
            internal::initial_literal_type(*call.operands[1]);
        if (!inferred) {
            const bool array_initial = call.operands[1]->kind == jsx::ExprKind::Array;
            internal::fail_hook(
                state, "TESSERA_IR_UNSUPPORTED",
                array_initial ? "an array initial needs the element type; write "
                                "`useState<T[]>(init)`"
                              : "`useState` needs a type argument when the initial value is "
                                "not a string, boolean or number literal",
                statement.span,
                array_initial ? "write `useState<string[]>([])`" : "write `useState<T>(init)`");
            return;
        }
        type = *inferred;
    }
    // ADR-051 slice 3: an object literal whose declared state type names a module record is a
    // record value (docs/specs/ir-v1.md section 4.3), not the generic "object literal is not an IR
    // expression" rejection. The record table decides; without one this stays the old error.
    ir::ExprPtr initial;
    const ir::RecordType* record = nullptr;
    if (state.context.records != nullptr) {
        if (const std::optional<std::string_view> name =
                internal::record_name_of(*state.context.records, type)) {
            record = internal::find_record(*state.context.records, *name);
        }
    }
    if (record != nullptr && call.operands[1]->kind == jsx::ExprKind::Object) {
        internal::RecordLoweringState record_state{state.context.source_map, state.context.file,
                                                   std::nullopt};
        initial = internal::lower_record_value(record_state, *call.operands[1], *record,
                                               *state.context.records);
        if (!initial) {
            if (!state.error && record_state.error) {
                state.error = std::move(record_state.error);
            }
            return;
        }
    } else {
        initial = internal::lower_component_expression(state, *call.operands[1], names);
        if (!initial) {
            return;
        }
    }
    if (is_list_type(type) && !validate_list_initial(state, type, *initial, statement.span)) {
        return;
    }
    out.state.push_back(ir::State{prefixed("state::", statement.names[0]), statement.names[0], type,
                                  std::move(initial)});
    out.setter_index.push_back({statement.names[1], prefixed("state::", statement.names[0])});
}

void lower_ref(HookState& state, std::size_t index, LoweredHooks& out) {
    const jsx::Statement& statement = state.context.component.body[index];
    const jsx::Expr& call = *statement.init;
    if (!statement.array_pattern && statement.names.size() == 1 && call.operands.size() == 2) {
        out.refs.push_back(
            ir::Ref{prefixed("ref::", statement.names[0]), statement.names[0], {}, {}});
        out.ref_index.push_back({statement.names[0], out.refs.back().id});
        return;
    }
    internal::fail_hook(state, "TESSERA_IR_MALFORMED",
                        "`useRef` must be `const r = useRef<Element>(null);`", statement.span);
}

// `useMergedRefs` (ADR-064 item 7, issue #630): one ref that fills several owners.
//
// A component that both forwards a ref to its parent and keeps an internal one has two owners for
// one node, and a node has one `ref` attribute. The hook is the answer: it declares a ref whose
// assignment also fills the refs it names, so the generated base writes one pointer into every
// owner.
//
// The lowering does not create a second ref: it records the aliases on the ref the component
// already has, because the merged ref IS that ref. Creating another would give the node two `ref`
// attributes and the second assignment would overwrite the first.
void lower_merged_refs(HookState& state, std::size_t index, LoweredHooks& out) {
    const jsx::Statement& statement = state.context.component.body[index];
    const jsx::Expr& call = *statement.init;
    if (statement.array_pattern || statement.names.size() != 1 || call.operands.size() < 3 ||
        !call.operands[1]) {
        internal::fail_hook(state, "TESSERA_IR_MALFORMED",
                            "`useMergedRefs` must be `const r = useMergedRefs(a, b);`",
                            statement.span);
        return;
    }
    // The first operand is the ref the merged ref stands for; the rest are the owners it also
    // fills.
    const jsx::Expr& primary = *call.operands[1];
    if (primary.kind != jsx::ExprKind::Identifier) {
        internal::fail_hook(state, "TESSERA_IR_MALFORMED", "`useMergedRefs` takes ref identifiers",
                            statement.span);
        return;
    }
    const std::string primary_id = prefixed("ref::", primary.text);
    ir::Ref* target = nullptr;
    for (ir::Ref& ref : out.refs) {
        if (ref.id == primary_id) {
            target = &ref;
            break;
        }
    }
    if (target == nullptr) {
        internal::fail_hook(state, "TESSERA_IR_UNRESOLVED",
                            "`useMergedRefs` names a ref that is not declared", statement.span);
        return;
    }
    // The remaining operands are the aliases. A ref that is not declared is an error rather than a
    // silent no-op: a forwarded ref that never fills is a bug the developer cannot see.
    for (std::size_t operand = 2; operand < call.operands.size(); ++operand) {
        const jsx::Expr* alias = call.operands[operand].get();
        if (alias == nullptr || alias->kind != jsx::ExprKind::Identifier) {
            internal::fail_hook(state, "TESSERA_IR_MALFORMED",
                                "`useMergedRefs` takes ref identifiers", statement.span);
            return;
        }
        const std::string alias_id = prefixed("ref::", alias->text);
        bool declared = false;
        for (const ir::Ref& ref : out.refs) {
            if (ref.id == alias_id) {
                declared = true;
                break;
            }
        }
        if (!declared) {
            internal::fail_hook(state, "TESSERA_IR_UNRESOLVED",
                                "`useMergedRefs` names a ref that is not declared", statement.span);
            return;
        }
        target->aliases.push_back(alias_id);
    }
}

void lower_animated(HookState& state, std::size_t index, LoweredHooks& out,
                    const ComponentNames& names) {
    const jsx::Statement& statement = state.context.component.body[index];
    const jsx::Expr& call = *statement.init;
    if (statement.array_pattern || statement.names.size() != 1 || call.operands.size() != 2 ||
        !call.operands[1]) {
        internal::fail_hook(state, "TESSERA_IR_MALFORMED",
                            "`useAnimatedValue` must be `const v = useAnimatedValue(init);`",
                            statement.span);
        return;
    }
    ir::ExprPtr initial = internal::lower_component_expression(state, *call.operands[1], names);
    if (!initial) {
        return;
    }
    // Animated values are sampled at the fixed timestep, so their IR type is the float of
    // docs/specs/ir-v1.md section 2; coercing the initial value is expression typing (slice 4).
    out.animated_values.push_back(ir::AnimatedValue{
        prefixed("anim::", statement.names[0]), statement.names[0], "float", std::move(initial)});
    out.animated_index.push_back({statement.names[0], out.animated_values.back().id});
}

HookLoweringResult error_result(HookState& state) {
    return std::unexpected(HookLoweringErrors{std::move(*state.error)});
}

} // namespace

HookLoweringResult lower_hooks(const HookLoweringContext& context) {
    HookState state{context, std::nullopt};
    if (context.analysis.item != &context.component) {
        internal::fail_hook(state, "TESSERA_IR_MALFORMED",
                            "the analysis belongs to a different component", {});
        return error_result(state);
    }
    if (!context.analysis.ok) {
        internal::fail_hook(state, "TESSERA_IR_MALFORMED",
                            "the component did not pass the analysis pass, so it has no IR",
                            context.component.span);
        return error_result(state);
    }
    LoweredHooks out;
    out.component_id = prefixed("comp::", context.component.name);
    const ComponentNames names(context.analysis);
    for (std::size_t index = 0; index < context.component.body.size(); ++index) {
        const jsx::Statement& statement = context.component.body[index];
        if (!statement.init) {
            internal::fail_hook(state, "TESSERA_IR_MALFORMED", "a statement has no expression",
                                statement.span);
            return error_result(state);
        }
        const std::string hook = internal::callee_name(*statement.init);
        if (hook == "useState") {
            lower_state(state, index, out, names);
        } else if (hook == "useReducer") {
            // ADR-066 item 4: one state cell whose dispatch table maps each action `type` to the
            // handler its case became. The analyzer resolved the form, so this emits it.
            internal::lower_reducer_hook(state, index, out, names);
        } else if (hook == "useRef") {
            lower_ref(state, index, out);
        } else if (hook == "useMergedRefs") {
            lower_merged_refs(state, index, out);
        } else if (hook == "useAnimatedValue") {
            lower_animated(state, index, out, names);
        } else if (hook == "useEffect" || hook == "useLayoutEffect") {
            // `useLayoutEffect` (ADR-066 item 1) has `useEffect` timing, so it lowers to the same
            // effect record; the analyzer already accepted it under the same form rules.
            internal::lower_effect_hook(state, index, out);
        } else if (hook == "useId") {
            // `useId` (ADR-066 item 1): the analyzer records it as an environment value with the
            // `id` provider, so the ordinary environment lowering produces its `environment[]`
            // entry. The generated base derives the value from the instance identity rather than
            // receiving it from the host.
            internal::lower_environment_hook(state, index, out, names);
        } else if (hook == "useService") {
            // ADR-050 section 3.1 (issue #177): one `services[]` entry; the analyzer resolved the
            // interface against the project manifest and recorded its C++ type and header.
            internal::lower_service_hook(state, index, out);
        } else if (hook == "useObservable") {
            internal::lower_observable_hook(state, index, out);
        } else if (hook == "useResource") {
            internal::lower_resource_hook(state, index, out, names);
        } else if (hook == "useDeferredValue") {
            internal::lower_deferred_value_hook(state, index, out, names);
        } else if (hook == "useContext") {
            // ADR-066 item 5 (issue #802): an environment entry naming the context, so the
            // provider walk in codegen resolves the value per instance.
            internal::lower_context_use(state, index, out, names);
        } else if (const jsx::HookEntry* behavior = rules::builtin_hook_manifest().find(hook);
                   behavior != nullptr && behavior->lowering == jsx::HookLoweringKind::Behavior) {
            internal::lower_behavior_hook(state, index, out, names);
        } else if (const jsx::HookEntry* environment = rules::builtin_hook_manifest().find(hook);
                   environment != nullptr &&
                   environment->lowering == jsx::HookLoweringKind::Environment) {
            internal::lower_environment_hook(state, index, out, names);
        } else if (statement.init->kind == jsx::ExprKind::Arrow) {
            internal::lower_declared_arrow_hook(state, index, out);
        }
        if (state.error) {
            return error_result(state);
        }
    }
    // docs/design/jsx-ir-lowering.md section 5: `bindings` is sorted by id. Every other list is
    // already in source order.
    std::sort(out.bindings.begin(), out.bindings.end(),
              [](const ir::Binding& left, const ir::Binding& right) { return left.id < right.id; });
    return out;
}

} // namespace tessera::compiler::lowering
