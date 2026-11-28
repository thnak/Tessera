// SPDX-License-Identifier: MIT

// Slice 9: attributes, callback props and children forwarding of a `Component` node
// (docs/specs/ir-v1.md section 3.4, docs/design/jsx-ir-lowering.md section 2, decision 7).
//
// A component reference is never inlined: its props keep their source attribute names with lowered
// value expressions, its callback props (`onX`) are events holding handler ids, and its children
// are the caller's static slot content (lowered in the caller's pre-order). Prop *types*, whether
// the target declares `children`, and the payload record of a callback prop belong to the type
// table (slice 4); until then the forms that would need them are rejected.

#include "node_lowering_internal.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

bool is_event_name(std::string_view name) {
    return name.size() > 2 && name[0] == 'o' && name[1] == 'n' && name[2] >= 'A' && name[2] <= 'Z';
}

bool lower_component_event(NodeState& state, std::string_view name, const jsx::Expr& value,
                           jsx::Span span, ir::Node& node) {
    if (value.kind == jsx::ExprKind::Arrow) {
        // A callback prop has no payload record in the intrinsic schema, so a parameter would
        // have no IR type until the type table lands (slice 4).
        if (!value.params.empty()) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "a callback prop of a component reference takes no parameter until the "
                      "component's prop types exist (slice 4)",
                      span);
            return false;
        }
        HandlerActionsResult actions = lower_handler_actions(value, state.context);
        if (!actions) {
            if (!state.error && !actions.error().empty()) {
                state.error = actions.error().front();
            }
            return false;
        }
        if (const std::optional<jsx::Diagnostic> violation =
                check_callback_calls(state.context, {}, *actions, value.span)) {
            state.error = violation;
            return false;
        }
        const std::string id = state.hooks.add_handler(
            {}, std::move(*actions),
            to_source_span(state.context.source_map, value.span, state.context.file));
        node.events[std::string(name)] = id;
        return true;
    }
    if (value.kind == jsx::ExprKind::Identifier) {
        if (const HandlerRef* handler = state.hooks.find_handler(value.text); handler != nullptr) {
            node.events[std::string(name)] = handler->id;
            return true;
        }
        // A payload-free callback prop of this component handed on to the child's callback prop.
        // The child's payload record is not known here (slice 4), so only `() => void` forwards.
        const ForwardedCallback forwarded =
            forward_callback_prop(state, value.text, name, {}, span);
        if (forwarded.matched) {
            if (forwarded.handler_id.empty()) {
                return false;
            }
            node.events[std::string(name)] = forwarded.handler_id;
            return true;
        }
        if (state.hooks.find_setter(value.text) != nullptr) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "a setter reference needs the callback payload type (slice 4)", span);
            return false;
        }
        fail_node(state, "TESSERA_IR_UNRESOLVED",
                  "`" + value.text + "` is not a handler of this component", span);
        return false;
    }
    fail_node(state, "TESSERA_IR_UNSUPPORTED",
              "a callback prop of a component reference must be a handler (subset 9)", span);
    return false;
}

bool lower_ref(NodeState& state, const jsx::Expr& value, jsx::Span span, ir::Node& node) {
    if (value.kind != jsx::ExprKind::Identifier) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED", "`ref` must name a `useRef`", span);
        return false;
    }
    if (state.hooks.find_ref(value.text) == nullptr) {
        fail_node(state, "TESSERA_IR_UNRESOLVED",
                  "`" + value.text + "` is not a `useRef` of this component", span);
        return false;
    }
    return state.hooks.set_ref_target(value.text, node.id);
}

bool check_prop_value(NodeState& state, const jsx::Expr& value, jsx::Span span) {
    if (value.kind == jsx::ExprKind::Jsx) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "a JSX element is not a prop value; component references are nodes (slice 9)",
                  span);
        return false;
    }
    if (classify_control(value) != ControlKind::None) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "a conditional or list loop cannot be an attribute value (slice 8)", span);
        return false;
    }
    return true;
}

bool apply_expression(NodeState& state, std::string_view name, const jsx::Expr& value,
                      jsx::Span span, ir::Node& node) {
    if (name == "children") {
        // Section 3.4: the slot is not an attribute; the caller writes it between the tags.
        fail_node(state, "TESSERA_IR_SLOT",
                  "`children` is a slot attribute on a component reference", span,
                  "write the nodes between `<Card>` and `</Card>`");
        return false;
    }
    if (is_event_name(name)) {
        return lower_component_event(state, name, value, span, node);
    }
    if (name == "key") {
        ir::ExprPtr key = lower_node_expression(state, value);
        if (!key) {
            return false;
        }
        node.key = std::move(key);
        return true;
    }
    if (name == "ref") {
        return lower_ref(state, value, span, node);
    }
    if (name == "automationId") {
        if (value.kind != jsx::ExprKind::String) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED", "`automationId` must be a string literal",
                      span);
            return false;
        }
        node.automation_id = value.text;
        return true;
    }
    if (name == "style") {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "`style` on a component reference needs the component's prop type (slice 4)",
                  span);
        return false;
    }
    // Issue #463: a `className` that reads a CSS module folds to its scoped class here too, so a
    // component reference accepts the same `styles.<class>` form as an intrinsic element.
    if (name == "className") {
        CssModuleClass css = resolve_css_module_class(state, value);
        if (css.kind == CssModuleClass::Kind::Error) {
            return false;
        }
        if (css.kind == CssModuleClass::Kind::Resolved) {
            node.props["className"] = ir::make_literal(std::move(css.text));
            return true;
        }
        if (css.kind == CssModuleClass::Kind::Expression) {
            node.props["className"] = std::move(css.expression);
            return true;
        }
    }
    // A component reference has no intrinsic schema: `onX` names are events by convention, and any
    // other attribute whose value names a declared handler (or is a payload-free inline arrow) is a
    // callback prop too (docs/specs/component-model-v1.md section 3, `Form`'s `validate`). The
    // target's declared prop record decides whether it is a callback; codegen rejects a value prop
    // bound as an event and vice versa.
    if (value.kind == jsx::ExprKind::Arrow || (value.kind == jsx::ExprKind::Identifier &&
                                               state.hooks.find_handler(value.text) != nullptr)) {
        return lower_component_event(state, name, value, span, node);
    }
    if (!check_prop_value(state, value, span)) {
        return false;
    }
    ir::ExprPtr lowered = lower_node_expression(state, value);
    if (!lowered) {
        return false;
    }
    node.props[std::string(name)] = std::move(lowered);
    return true;
}

bool apply_literal(NodeState& state, std::string_view name, ir::ExprPtr value, jsx::Span span,
                   ir::Node& node) {
    if (name == "children") {
        fail_node(state, "TESSERA_IR_SLOT",
                  "`children` is a slot attribute on a component reference", span,
                  "write the nodes between `<Card>` and `</Card>`");
        return false;
    }
    if (is_event_name(name)) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "a callback prop of a component reference must be a handler (subset 9)", span);
        return false;
    }
    if (name == "key") {
        node.key = std::move(value);
        return true;
    }
    if (name == "ref") {
        fail_node(state, "TESSERA_IR_UNSUPPORTED", "`ref` needs a `useRef` name", span);
        return false;
    }
    if (name == "automationId") {
        if (value->kind != ir::ExprKind::Literal ||
            !std::holds_alternative<std::string>(value->literal)) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED", "`automationId` must be a string literal",
                      span);
            return false;
        }
        node.automation_id = std::get<std::string>(value->literal);
        return true;
    }
    if (name == "style") {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "`style` on a component reference needs the component's prop type (slice 4)",
                  span);
        return false;
    }
    node.props[std::string(name)] = std::move(value);
    return true;
}

bool lower_spread(NodeState& state, const jsx::JsxAttribute& attribute, ir::Node& node) {
    if (!attribute.expr) {
        fail_node(state, "TESSERA_IR_MALFORMED", "a spread attribute has no expression",
                  attribute.span);
        return false;
    }
    if (attribute.expr->kind != jsx::ExprKind::Object) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "a spread of the props parameter needs the props type and is slice 4/9",
                  attribute.span, "spread an object literal");
        return false;
    }
    for (const jsx::ObjectProperty& property : attribute.expr->properties) {
        if (!property.value) {
            fail_node(state, "TESSERA_IR_MALFORMED", "a spread property has no value",
                      property.key_span);
            return false;
        }
        if (!apply_expression(state, property.key, *property.value, property.key_span, node)) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace {

// Attributes every lowered tag keeps as written: they are not HTML attributes of the control but
// the framework's own (key, ref, className, automationId).
bool is_framework_attribute(std::string_view name) {
    return name == "key" || name == "ref" || name == "className" || name == "automationId";
}

// Issue #1041: the component prop an HTML attribute of a library-lowered tag becomes. Empty when
// the attribute is dropped (`type` of an `input`); nullopt after reporting a rejected one.
std::optional<std::string_view> library_attribute_name(NodeState& state, const ElementInfo& info,
                                                       const jsx::JsxAttribute& attribute) {
    const schema::LibraryLowering& lowering = *info.library;
    if (is_framework_attribute(attribute.name)) {
        return std::string_view(attribute.name);
    }
    if (attribute.name == "type" && lowering.tag == "input") {
        return std::string_view();
    }
    for (const schema::LibraryAttr& row : lowering.attrs) {
        if (row.name != attribute.name) {
            continue;
        }
        if (row.rename.empty()) {
            fail_node(state, "TESSERA_JSX_SUBSET_HTML_ATTR",
                      "`" + attribute.name + "` has no meaning on `<" + std::string(lowering.tag) +
                          ">` in V1",
                      attribute.span, std::string(row.suggestion));
            return std::nullopt;
        }
        return row.rename;
    }
    fail_node(state, "TESSERA_JSX_SUBSET_HTML_ATTR",
              "`" + attribute.name + "` is not an attribute of `<" + std::string(lowering.tag) +
                  ">`, which lowers onto the library `" + std::string(lowering.component) + "`",
              attribute.span, "use `className` and the attributes the typings list");
    return std::nullopt;
}

// `<details>`: the first `<summary>` child (static text only) is the Disclosure's `summary` prop.
bool lower_details_summary(NodeState& state, const jsx::JsxNode& element, ir::Node& node) {
    for (const jsx::JsxPtr& child : element.children) {
        if (!child || child->kind != jsx::JsxKind::Element || child->tag != "summary") {
            continue;
        }
        std::string text;
        for (const jsx::JsxPtr& part : child->children) {
            if (!part || part->kind != jsx::JsxKind::Text) {
                fail_node(state, "TESSERA_IR_UNSUPPORTED",
                          "the `<summary>` of a `<details>` holds static text only in V1",
                          part ? part->span : child->span,
                          "use the library `Disclosure` for a richer summary");
                return false;
            }
            text += part->text;
        }
        if (!child->attributes.empty()) {
            fail_node(state, "TESSERA_JSX_SUBSET_HTML_ATTR",
                      "a `<summary>` takes no attributes in V1", child->attributes.front().span);
            return false;
        }
        node.props["summary"] = ir::make_literal(std::move(text));
        return true;
    }
    fail_node(state, "TESSERA_IR_UNSUPPORTED", "a `<details>` needs a `<summary>` child",
              element.span, "add `<summary>Title</summary>` as the first child");
    return false;
}

} // namespace

bool lower_component_attributes(NodeState& state, const jsx::JsxNode& element,
                                const ElementInfo& info, ir::Node& node) {
    for (const jsx::JsxAttribute& attribute : element.attributes) {
        std::string_view name = attribute.name;
        if (info.library != nullptr && attribute.kind != jsx::JsxAttrKind::Spread) {
            const std::optional<std::string_view> renamed =
                library_attribute_name(state, info, attribute);
            if (!renamed) {
                return false;
            }
            if (renamed->empty()) {
                continue;
            }
            name = *renamed;
        }
        switch (attribute.kind) {
        case jsx::JsxAttrKind::Spread:
            if (!lower_spread(state, attribute, node)) {
                return false;
            }
            break;
        case jsx::JsxAttrKind::Flag:
            if (!apply_literal(state, name, ir::make_literal(true), attribute.span, node)) {
                return false;
            }
            break;
        case jsx::JsxAttrKind::String:
            if (!apply_literal(state, name, ir::make_literal(attribute.string_value),
                               attribute.span, node)) {
                return false;
            }
            break;
        case jsx::JsxAttrKind::Expression:
            if (!attribute.expr) {
                fail_node(state, "TESSERA_IR_MALFORMED", "an attribute has no expression",
                          attribute.span);
                return false;
            }
            if (!apply_expression(state, name, *attribute.expr, attribute.span, node)) {
                return false;
            }
            break;
        }
    }
    if (info.library != nullptr && info.library->tag == "details") {
        return lower_details_summary(state, element, node);
    }
    return true;
}

} // namespace tessera::compiler::lowering::internal
