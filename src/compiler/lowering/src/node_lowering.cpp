// SPDX-License-Identifier: MIT

// Slice 7 entry point: the returned JSX element of one analysed component becomes the IR node tree
// of docs/specs/ir-v1.md section 3, on top of the slice 6 hook and binding data.

#include "node_lowering_internal.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {

std::optional<ir::Node> make_node(NodeState& state, const jsx::JsxNode& element) {
    std::optional<ElementInfo> info = resolve_element(state, element);
    if (!info) {
        return std::nullopt;
    }
    ir::Node node;
    node.kind = info->kind;
    node.id = next_node_id(state);
    node.source = to_source_span(state.context.source_map, element.span, state.context.file);
    if (info->is_component) {
        node.component = info->component_id;
        if (!lower_component_attributes(state, element, *info, node)) {
            return std::nullopt;
        }
        if (!lower_slot_children(state, element, node)) {
            return std::nullopt;
        }
        return node;
    }
    if (!lower_attributes(state, element, *info, node)) {
        return std::nullopt;
    }
    if (!lower_children(state, element, *info, node)) {
        return std::nullopt;
    }
    return node;
}

} // namespace tessera::compiler::lowering::internal

namespace tessera::compiler::lowering {
namespace {

NodeLoweringErrors single_error(const jsx::Diagnostic& diagnostic) {
    NodeLoweringErrors errors;
    errors.push_back(diagnostic);
    return errors;
}

} // namespace

LoweredComponentResult lower_component(const HookLoweringContext& context) {
    HookLoweringResult hooks = lower_hooks(context);
    if (!hooks) {
        return std::unexpected(std::move(hooks.error()));
    }
    internal::NodeState state{context, *hooks};
    if (!context.component.returned) {
        internal::fail_node(state, "TESSERA_IR_MALFORMED", "the component has no returned element",
                            context.component.span);
        return std::unexpected(single_error(*state.error));
    }
    const jsx::JsxNode& returned = *context.component.returned;
    if (returned.kind != jsx::JsxKind::Element) {
        internal::fail_node(state, "TESSERA_IR_UNSUPPORTED",
                            "a component must return exactly one element; a fragment is spliced "
                            "only as a child (jsx-subset-v1.md section 10.1)",
                            returned.span, "wrap the children in one element");
        return std::unexpected(single_error(*state.error));
    }
    std::optional<ir::Node> root = internal::make_node(state, returned);
    if (!root) {
        return std::unexpected(single_error(*state.error));
    }
    LoweredComponent lowered;
    ir::Component& component = lowered.component;
    component.id = "comp::" + context.component.name;
    component.name = context.component.name;
    component.source = to_source_span(context.source_map, context.component.span, context.file);
    // Slice 4: the declared prop records come from the props parameter's type, inline or through a
    // same-file alias. Without a resolvable object type only the static `children` slot is
    // recorded, which is the pre-slice-4 behavior for the component-level entry points.
    const internal::DeclaredPropsKind declared =
        internal::lower_declared_props(state, component.props);
    if (declared == internal::DeclaredPropsKind::Error) {
        return std::unexpected(single_error(*state.error));
    }
    if (declared == internal::DeclaredPropsKind::None) {
        for (const jsx::PropBinding& binding : context.component.props_bindings) {
            if (binding.name != "children") {
                continue;
            }
            if (binding.default_value) {
                internal::fail_node(state, "TESSERA_IR_SLOT",
                                    "the `children` prop has no default value", binding.span);
                return std::unexpected(single_error(*state.error));
            }
            component.props.push_back(ir::Prop{"children", "children", nullptr});
        }
    }
    component.state = std::move(hooks->state);
    component.handlers = std::move(hooks->handlers);
    component.effects = std::move(hooks->effects);
    component.refs = std::move(hooks->refs);
    component.animated_values = std::move(hooks->animated_values);
    component.root = std::move(*root);
    lowered.bindings = std::move(hooks->bindings);
    return lowered;
}

} // namespace tessera::compiler::lowering
