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
