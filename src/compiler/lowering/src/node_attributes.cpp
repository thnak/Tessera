// SPDX-License-Identifier: MIT

// JSX attributes of slice 7 (docs/specs/jsx-subset-v1.md section 10.2): string, expression and flag
// values, spread of an object literal, `key`, `ref`, `className`, `style`, `automationId` and the
// intrinsic prop and event vocabulary of the shared schema table.

#include "node_lowering_internal.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tessera::compiler::lowering::internal {
namespace {

bool is_literal(const jsx::Expr& expr) {
    return expr.kind == jsx::ExprKind::Number || expr.kind == jsx::ExprKind::String ||
           expr.kind == jsx::ExprKind::Boolean;
}

void put_prop(ir::Node& node, std::string_view name, ir::ExprPtr value) {
    node.props[std::string(name)] = std::move(value);
}

bool lower_ref(NodeState& state, const ElementInfo& element, const jsx::Expr& value, jsx::Span span,
               ir::Node& node) {
    if (element.row->ref_type.empty()) {
        fail_node(state, "TESSERA_IR_UNKNOWN_PROP",
                  "`ref` is not accepted on `<" + std::string(element.row->tag) + ">`", span);
        return false;
    }
    if (value.kind != jsx::ExprKind::Identifier) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED", "`ref` must name a `useRef`", span,
                  "write `ref={r}` with `const r = useRef<T>(null)`");
        return false;
    }
    if (state.hooks.find_ref(value.text) == nullptr) {
        fail_node(state, "TESSERA_IR_UNRESOLVED",
                  "`" + value.text + "` is not a `useRef` of this component", span);
        return false;
    }
    return state.hooks.set_ref_target(value.text, node.id);
}

// `tabIndex` takes `{0}` or `{-1}` (issue #226): the focus order is a static property of the tree,
// and a positive value would reorder the whole Tab sequence, so it has no V1 spelling.
bool apply_tab_index(NodeState& state, const jsx::Expr& value, jsx::Span span, ir::Node& node) {
    std::optional<std::int64_t> parsed;
    if (value.kind == jsx::ExprKind::Number && value.number == 0.0) {
        parsed = 0;
    } else if (value.kind == jsx::ExprKind::Unary && value.text == "-" &&
               value.operands.size() == 1 && value.operands[0] &&
               value.operands[0]->kind == jsx::ExprKind::Number &&
               value.operands[0]->number == 1.0) {
        parsed = -1;
    }
    if (!parsed) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED", "`tabIndex` accepts the literal `0` or `-1`",
                  span,
                  "write `tabIndex={0}` to join the Tab order or `tabIndex={-1}` for programmatic "
                  "focus only");
        return false;
    }
    put_prop(node, "tabIndex", ir::make_literal(*parsed));
    return true;
}

// A normal prop value: the schema value class decides whether a literal is required, and the rest
// is a lowered expression (types are checked by slice 4).
bool apply_prop_value(NodeState& state, const schema::PropRow& prop, const jsx::Expr& value,
                      jsx::Span span, ir::Node& node) {
    if (prop.name == "tabIndex") {
        return apply_tab_index(state, value, span, node);
    }
    switch (prop.value) {
    case schema::ValueKind::Literal:
        if (!is_literal(value)) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`" + std::string(prop.name) + "` accepts a literal only", span);
            return false;
        }
        break;
    case schema::ValueKind::Expression:
        break;
    case schema::ValueKind::Handler:
    case schema::ValueKind::Element:
    case schema::ValueKind::Name:
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "`" + std::string(prop.name) + "` has no plain expression form", span);
        return false;
    }
    ir::ExprPtr lowered = lower_node_expression(state, value);
    if (!lowered) {
        return false;
    }
    put_prop(node, prop.name, std::move(lowered));
    return true;
}

bool is_string_literal(const ir::Expr& expr) {
    return expr.kind == ir::ExprKind::Literal && std::holds_alternative<std::string>(expr.literal);
}

bool apply_expression_attribute(NodeState& state, const ElementInfo& element, std::string_view name,
                                const jsx::Expr& value, jsx::Span span, ir::Node& node) {
    if (classify_control(value) != ControlKind::None) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "a conditional or list loop cannot be an attribute value (slice 8)", span);
        return false;
    }
    const AttributeInfo info = resolve_attribute(element, name);
    switch (info.kind) {
    case AttributeInfo::Kind::Event:
        return lower_event_value(state, element, *info.event, value, span, node);
    case AttributeInfo::Kind::Key: {
        ir::ExprPtr key = lower_node_expression(state, value);
        if (!key) {
            return false;
        }
        node.key = std::move(key);
        return true;
    }
    case AttributeInfo::Kind::Ref:
        return lower_ref(state, element, value, span, node);
    case AttributeInfo::Kind::ClassName: {
        if (!element.row->accepts_class_name) {
            fail_node(state, "TESSERA_IR_UNKNOWN_PROP",
                      "`className` is not accepted on `<" + std::string(element.row->tag) + ">`",
                      span);
            return false;
        }
        ir::ExprPtr lowered = lower_node_expression(state, value);
        if (!lowered) {
            return false;
        }
        put_prop(node, "className", std::move(lowered));
        return true;
    }
    case AttributeInfo::Kind::Style:
        if (!element.row->accepts_style) {
            fail_node(state, "TESSERA_IR_UNKNOWN_PROP",
                      "`style` is not accepted on `<" + std::string(element.row->tag) + ">`", span);
            return false;
        }
        return lower_style(state, value, node);
    case AttributeInfo::Kind::AutomationId:
        if (value.kind != jsx::ExprKind::String) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED", "`automationId` must be a string literal",
                      span);
            return false;
        }
        node.automation_id = value.text;
        return true;
    case AttributeInfo::Kind::CommonProp:
        return apply_prop_value(state, *info.prop, value, span, node);
    case AttributeInfo::Kind::ElementProp:
        if (info.prop->value != schema::ValueKind::Element) {
            return apply_prop_value(state, *info.prop, value, span, node);
        }
        // The one element-valued attribute in V1 is ErrorBoundary's `fallback`, which has its own
        // IR node field (`Node::fallback`).
        if (element.row->tag != "ErrorBoundary" || info.prop->name != "fallback") {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`" + std::string(info.prop->name) + "` has no IR field", span);
            return false;
        }
        if (value.kind != jsx::ExprKind::Jsx || !value.jsx) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED", "`fallback` needs an element value", span,
                      "write `fallback={<View />}`");
            return false;
        }
        if (value.jsx->kind != jsx::JsxKind::Element) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`fallback` must be one element, not a fragment", value.jsx->span);
            return false;
        }
        if (std::optional<ir::Node> fallback = make_node(state, *value.jsx); fallback) {
            node.fallback = std::make_unique<ir::Node>(std::move(*fallback));
            return true;
        }
        return false;
    case AttributeInfo::Kind::UnknownEvent:
        fail_node(state, "TESSERA_IR_UNKNOWN_PROP",
                  "`" + std::string(name) + "` is not an event of `<" +
                      std::string(element.row->tag) + ">`",
                  span);
        return false;
    case AttributeInfo::Kind::UnknownProp:
        break;
    }
    fail_node(state, "TESSERA_IR_UNKNOWN_PROP",
              "`" + std::string(name) + "` is not a prop of `<" + std::string(element.row->tag) +
                  ">`",
              span);
    return false;
}

bool apply_literal_attribute(NodeState& state, const ElementInfo& element, std::string_view name,
                             ir::ExprPtr value, jsx::Span span, ir::Node& node) {
    const AttributeInfo info = resolve_attribute(element, name);
    switch (info.kind) {
    case AttributeInfo::Kind::Event:
        fail_node(state, "TESSERA_IR_UNSUPPORTED", "an event value must be a handler (subset 9)",
                  span);
        return false;
    case AttributeInfo::Kind::Key:
        node.key = std::move(value);
        return true;
    case AttributeInfo::Kind::Ref:
        fail_node(state, "TESSERA_IR_UNSUPPORTED", "`ref` needs a `useRef` name", span);
        return false;
    case AttributeInfo::Kind::ClassName:
        if (!element.row->accepts_class_name) {
            fail_node(state, "TESSERA_IR_UNKNOWN_PROP",
                      "`className` is not accepted on `<" + std::string(element.row->tag) + ">`",
                      span);
            return false;
        }
        put_prop(node, "className", std::move(value));
        return true;
    case AttributeInfo::Kind::Style:
        if (!element.row->accepts_style) {
            fail_node(state, "TESSERA_IR_UNKNOWN_PROP",
                      "`style` is not accepted on `<" + std::string(element.row->tag) + ">`", span);
            return false;
        }
        fail_node(state, "TESSERA_IR_UNSUPPORTED", "`style` needs an object literal", span);
        return false;
    case AttributeInfo::Kind::AutomationId:
        if (!is_string_literal(*value)) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED", "`automationId` must be a string literal",
                      span);
            return false;
        }
        node.automation_id = std::get<std::string>(value->literal);
        return true;
    case AttributeInfo::Kind::CommonProp:
        put_prop(node, info.prop->name, std::move(value));
        return true;
    case AttributeInfo::Kind::ElementProp:
        if (info.prop->value == schema::ValueKind::Element ||
            info.prop->value == schema::ValueKind::Handler ||
            info.prop->value == schema::ValueKind::Name) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`" + std::string(info.prop->name) + "` has no literal form", span);
            return false;
        }
        if (info.prop->name == "tabIndex") {
            fail_node(state, "TESSERA_IR_UNSUPPORTED", "`tabIndex` accepts the literal `0` or `-1`",
                      span, "write `tabIndex={0}` or `tabIndex={-1}`");
            return false;
        }
        if (info.prop->name == "autoFocus" && !std::holds_alternative<bool>(value->literal)) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED", "`autoFocus` is a boolean flag", span,
                      "write `autoFocus` or `autoFocus={true}`");
            return false;
        }
        if (info.prop->name == "focusTrap" && !std::holds_alternative<bool>(value->literal)) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED", "`focusTrap` is a boolean flag", span,
                      "write `focusTrap` or `focusTrap={true}`");
            return false;
        }
        if (info.prop->name == "focusGroup") {
            const auto* axis = std::get_if<std::string>(&value->literal);
            if (axis == nullptr ||
                (*axis != "horizontal" && *axis != "vertical" && *axis != "both")) {
                fail_node(state, "TESSERA_IR_UNSUPPORTED",
                          "`focusGroup` is \"horizontal\", \"vertical\" or \"both\"", span);
                return false;
            }
        }
        put_prop(node, info.prop->name, std::move(value));
        return true;
    case AttributeInfo::Kind::UnknownEvent:
        fail_node(state, "TESSERA_IR_UNKNOWN_PROP",
                  "`" + std::string(name) + "` is not an event of `<" +
                      std::string(element.row->tag) + ">`",
                  span);
        return false;
    case AttributeInfo::Kind::UnknownProp:
        break;
    }
    fail_node(state, "TESSERA_IR_UNKNOWN_PROP",
              "`" + std::string(name) + "` is not a prop of `<" + std::string(element.row->tag) +
                  ">`",
              span);
    return false;
}

// `{...expr}`: only an object literal has a statically known key set; a spread of the props
// parameter needs the props type of slice 4/9.
bool lower_spread(NodeState& state, const ElementInfo& element, const jsx::JsxAttribute& attribute,
                  ir::Node& node) {
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
        if (!apply_expression_attribute(state, element, property.key, *property.value,
                                        property.key_span, node)) {
            return false;
        }
    }
    return true;
}

bool lower_attribute(NodeState& state, const ElementInfo& element,
                     const jsx::JsxAttribute& attribute, ir::Node& node) {
    switch (attribute.kind) {
    case jsx::JsxAttrKind::Spread:
        return lower_spread(state, element, attribute, node);
    case jsx::JsxAttrKind::Flag:
        return apply_literal_attribute(state, element, attribute.name, ir::make_literal(true),
                                       attribute.span, node);
    case jsx::JsxAttrKind::String:
        return apply_literal_attribute(state, element, attribute.name,
                                       ir::make_literal(attribute.string_value), attribute.span,
                                       node);
    case jsx::JsxAttrKind::Expression:
        if (!attribute.expr) {
            fail_node(state, "TESSERA_IR_MALFORMED", "an attribute has no expression",
                      attribute.span);
            return false;
        }
        return apply_expression_attribute(state, element, attribute.name, *attribute.expr,
                                          attribute.span, node);
    }
    fail_node(state, "TESSERA_IR_MALFORMED", "unknown attribute kind", attribute.span);
    return false;
}

} // namespace

bool lower_attributes(NodeState& state, const jsx::JsxNode& element, const ElementInfo& info,
                      ir::Node& node) {
    for (const jsx::JsxAttribute& attribute : element.attributes) {
        if (!lower_attribute(state, info, attribute, node)) {
            return false;
        }
    }
    return true;
}

} // namespace tessera::compiler::lowering::internal
