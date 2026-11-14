// SPDX-License-Identifier: MIT

// Slice 7 entry point: the returned JSX element of one analysed component becomes the IR node tree
// of docs/specs/ir-v1.md section 3, on top of the slice 6 hook and binding data.

#include "node_lowering_internal.hpp"
#include "tessera/compiler/lowering/source_span.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

// A name comes from `aria-label`, or from text anywhere in the subtree (the accessible name of a
// button-like role is its content, docs/specs/accessibility-v1.md section 2 name precedence).
bool has_accessible_name(const ir::Node& node) {
    if (node.props.find("aria-label") != node.props.end() || node.text != nullptr) {
        return true;
    }
    return std::any_of(node.children.begin(), node.children.end(),
                       [](const ir::Node& child) { return has_accessible_name(child); });
}

// Issue #226: a focusable `View`/`Text` (`tabIndex`) must declare semantics, because inference is
// allowed only for text and labels (accessibility-v1 G-18). `autoFocus` has no meaning without a
// focusable node.
bool check_focusable_semantics(NodeState& state, const jsx::JsxNode& element,
                               const ir::Node& node) {
    const bool focusable = node.props.find("tabIndex") != node.props.end();
    if (node.props.find("autoFocus") != node.props.end() && !focusable) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED", "`autoFocus` needs `tabIndex` on the same node",
                  element.span, "add `tabIndex={0}`");
        return false;
    }
    if (!focusable) {
        return true;
    }
    if (node.props.find("role") == node.props.end()) {
        fail_node(state, "TESSERA_JSX_FOCUSABLE_NO_ROLE",
                  "a focusable node must declare its role: inference is allowed only for text and "
                  "labels",
                  element.span, "add `role=\"button\"` (or the role that matches the control)");
        return false;
    }
    if (!has_accessible_name(node)) {
        fail_node(state, "TESSERA_JSX_FOCUSABLE_NO_NAME",
                  "a focusable node must have an accessible name", element.span,
                  "add `aria-label=\"...\"` or put text inside the node");
        return false;
    }
    return true;
}

// Alias defaults inside a Text span (issue #229): injected props (`a` -> role=link) and the
// injected run style (`b` -> font-weight: 700). Explicit attributes and inline style declarations
// win, so a default is only added when the slot is still empty.
void apply_alias_defaults(const ElementInfo& info, ir::Node& node) {
    for (const schema::AliasInject& inject : info.inject) {
        const std::string name(inject.name);
        if (node.props.find(name) == node.props.end()) {
            node.props.emplace(name, ir::make_literal(std::string(inject.value)));
        }
    }
    for (const schema::AliasStyle& style : info.style_defaults) {
        const std::string name(style.property);
        if (node.style.find(name) == node.style.end()) {
            node.style.emplace(name, ir::make_literal(std::string(style.value)));
        }
    }
}

} // namespace

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
    apply_alias_defaults(*info, node);
    if (!lower_children(state, element, *info, node)) {
        return std::nullopt;
    }
    if (!check_focusable_semantics(state, element, node)) {
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
    // type alias. The alias may be declared in another project file (issue #70): `types` resolves
    // imports, renames and re-export chains. Without a resolvable object type only the static
    // `children` slot is recorded, which is the pre-slice-4 behavior for the component-level entry
    // points.
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
