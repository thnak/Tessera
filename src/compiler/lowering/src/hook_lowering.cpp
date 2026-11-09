// SPDX-License-Identifier: MIT

// Slice 6 entry point: one analysed component's hook declarations and `bind::` bindings become the
// IR component data of docs/specs/ir-v1.md sections 2, 2.1 and 6. JSX nodes are slice 7 and are not
// read here (the returned element is ignored).

#include "hook_lowering_internal.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::lowering {
namespace {

using internal::HookState;

std::string prefixed(std::string_view prefix, const std::string& name) {
    return std::string(prefix) + name;
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
            internal::fail_hook(state, "TESSERA_IR_UNSUPPORTED",
                                "`useState` needs a type argument when the initial value is not a "
                                "string, boolean or number literal",
                                statement.span, "write `useState<T>(init)`");
            return;
        }
        type = *inferred;
    }
    ir::ExprPtr initial = internal::lower_component_expression(state, *call.operands[1], names);
    if (!initial) {
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
        out.refs.push_back(ir::Ref{prefixed("ref::", statement.names[0]), statement.names[0], {}});
        out.ref_index.push_back({statement.names[0], out.refs.back().id});
        return;
    }
    internal::fail_hook(state, "TESSERA_IR_MALFORMED",
                        "`useRef` must be `const r = useRef<Element>(null);`", statement.span);
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
        } else if (hook == "useRef") {
            lower_ref(state, index, out);
        } else if (hook == "useAnimatedValue") {
            lower_animated(state, index, out, names);
        } else if (hook == "useEffect") {
            internal::lower_effect_hook(state, index, out);
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
