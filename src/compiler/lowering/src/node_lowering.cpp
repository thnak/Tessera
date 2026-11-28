// SPDX-License-Identifier: MIT

// Slice 7 entry point: the returned JSX element of one analysed component becomes the IR node tree
// of docs/specs/ir-v1.md section 3, on top of the slice 6 hook and binding data.

#include "node_lowering_internal.hpp"
#include "record_internal.hpp"
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
    if (!info.alias_tag.empty() && node.props.find("tag") == node.props.end()) {
        node.props.emplace("tag", ir::make_literal(std::string(info.alias_tag)));
    }
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
    if (!info->provider.empty()) {
        // ADR-066 item 5 (issue #802): a `<Ctx.Provider value={...}>` wrapper. The node is a View
        // holding the provided subtree; the provider fields name the context and carry the value,
        // which must be a pure expression of this component's props and state. No other attribute
        // exists on a provider, and the wrapper has no style or key of its own.
        node.provider = info->provider;
        const jsx::JsxAttribute* value = nullptr;
        for (const jsx::JsxAttribute& attribute : element.attributes) {
            if (attribute.name == "value") {
                value = &attribute;
                break;
            }
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "a `<" + element.tag + ">` provider carries only `value`", attribute.span);
            return std::nullopt;
        }
        if (value == nullptr) {
            fail_node(state, "TESSERA_IR_MALFORMED",
                      "a `<" + element.tag + ">` provider needs its `value`", element.tag_span);
            return std::nullopt;
        }
        if (value->kind != jsx::JsxAttrKind::Expression || value->expr == nullptr) {
            fail_node(state, "TESSERA_IR_MALFORMED",
                      "the `value` of a `<" + element.tag +
                          ">` provider is an expression attribute",
                      value->span);
            return std::nullopt;
        }
        // The value's type is the context's: a record-typed context takes an object literal, which
        // lowers to an `ir::Record` â€” the same rule a record-typed prop default follows (ADR-051
        // slice 3) â€” and a primitive context takes an ordinary pure expression.
        const rules::ContextInfo* declared =
            state.context.analyzed != nullptr ? state.context.analyzed->find_context(info->provider)
                                              : nullptr;
        const ir::RecordType* record = nullptr;
        if (declared != nullptr && state.context.records != nullptr) {
            if (const std::optional<std::string_view> name =
                    record_name_of(*state.context.records, declared->ir_type)) {
                record = find_record(*state.context.records, *name);
            }
        }
        if (record != nullptr) {
            if (value->expr->kind != jsx::ExprKind::Object) {
                fail_node(state, "TESSERA_IR_RECORD_VALUE",
                          "the `value` of a `<" + element.tag + ">` provider is type `" +
                              record->name + "`, so it must be an object literal",
                          value->span);
                return std::nullopt;
            }
            // The names resolver turns a shorthand field (`{ accent }`) into the dotted IR id it
            // reads (`prop::accent`), exactly as a record-typed prop default does.
            RecordLoweringState record_state{state.context.source_map, state.context.file,
                                             std::nullopt, &state.names};
            ir::ExprPtr lowered =
                lower_record_value(record_state, *value->expr, *record, *state.context.records);
            if (!lowered) {
                if (!state.error && record_state.error) {
                    state.error = std::move(record_state.error);
                }
                if (!state.error) {
                    fail_node(state, "TESSERA_IR_RECORD_VALUE",
                              "the `value` of a `<" + element.tag + ">` provider does not lower",
                              value->span);
                }
                return std::nullopt;
            }
            node.provider_value = std::move(lowered);
        } else {
            ir::ExprPtr lowered = lower_node_expression(state, *value->expr);
            if (!lowered) {
                return std::nullopt;
            }
            node.provider_value = std::move(lowered);
        }
        // The provider wrapper's children are the provided subtree: any elements, in order —
        // `Multiple` semantics, but built directly because a provider has no schema row to read
        // the children kind from.
        std::vector<ChildItem> items;
        if (!flatten_children(state, element, items, false)) {
            return std::nullopt;
        }
        if (!append_runs_and_elements(state, items, node)) {
            return std::nullopt;
        }
        return node;
    }
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

LoweredComponentResult lower_component(const HookLoweringContext& base_context) {
    // The declared props come first (issue #286): forwarding or calling a callback prop needs its
    // type, in the handlers of the hooks as well as in the nodes.
    std::vector<ir::Prop> declared_props;
    LoweredHooks props_hooks;
    internal::NodeState props_state{base_context, props_hooks};
    const internal::DeclaredPropsKind declared =
        internal::lower_declared_props(props_state, declared_props);
    if (declared == internal::DeclaredPropsKind::Error) {
        return std::unexpected(single_error(*props_state.error));
    }
    HookLoweringContext context = base_context;
    if (declared == internal::DeclaredPropsKind::Lowered) {
        context.declared_props = &declared_props;
    }
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
    // ADR-066 item 1: a `StrictMode`/`Profiler` root is a pass-through wrapper, so the component
    // returns its single child instead. More than one child has no single root, which is the same
    // rule a fragment root gets.
    const jsx::JsxNode* root_element = &returned;
    while (internal::is_passthrough_element(*root_element)) {
        if (root_element->children.size() != 1 || !root_element->children.front() ||
            root_element->children.front()->kind != jsx::JsxKind::Element) {
            internal::fail_node(state, "TESSERA_IR_UNSUPPORTED",
                                "a component must return exactly one element; `" +
                                    root_element->tag +
                                    "` is a pass-through wrapper, so it must hold one element",
                                root_element->span, "wrap the children in one element");
            return std::unexpected(single_error(*state.error));
        }
        root_element = root_element->children.front().get();
    }
    if (root_element->kind != jsx::JsxKind::Element) {
        internal::fail_node(state, "TESSERA_IR_UNSUPPORTED",
                            "a component must return exactly one element; a fragment is spliced "
                            "only as a child (jsx-subset-v1.md section 10.1)",
                            root_element->span, "wrap the children in one element");
        return std::unexpected(single_error(*state.error));
    }
    std::optional<ir::Node> root = internal::make_node(state, *root_element);
    if (!root) {
        return std::unexpected(single_error(*state.error));
    }
    LoweredComponent lowered;
    ir::Component& component = lowered.component;
    component.props = std::move(declared_props);
    component.id = "comp::" + context.component.name;
    component.name = context.component.name;
    component.source = to_source_span(context.source_map, context.component.span, context.file);
    // Slice 4: the declared prop records come from the props parameter's type, inline or through a
    // type alias. The alias may be declared in another project file (issue #70): `types` resolves
    // imports, renames and re-export chains. Without a resolvable object type only the static
    // `children` slot is recorded, which is the pre-slice-4 behavior for the component-level entry
    // points.
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
    // ADR-064 item 7: the behavior hook calls. Without this the lowering produced `behaviors` and
    // dropped them on the floor, so no component ever carried one and the whole behavior path --
    // code generation, the preview, the generated accessors -- was unreachable.
    component.behaviors = std::move(hooks->behaviors);
    // `environment` hook values (docs/specs/jsx-subset-v1.md section 7.1): the same rule as
    // `behaviors` above. Without this the lowering produced them and dropped them, so no component
    // ever carried one and `useBreakpoint`/`useTheme` were unreachable.
    component.environment = std::move(hooks->environment);
    // ADR-050 section 3.1 (issue #177): the `useService<T>()` requirements. Codegen resolves each
    // once at mount, and a `callService` action names one by its `svc::` id.
    component.services = std::move(hooks->services);
    component.resources = std::move(hooks->resources);
    component.root = std::move(*root);
    lowered.bindings = std::move(hooks->bindings);
    // Issue #463: the CSS module reads belong to the module pass, which compares them against every
    // scanned sheet to report a class no TSX reads.
    lowered.css_module_uses = std::move(state.css_module_uses);
    return lowered;
}

} // namespace tessera::compiler::lowering
