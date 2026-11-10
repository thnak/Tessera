// SPDX-License-Identifier: MIT

// Shared helpers of node lowering: first-error reporting, node ids, tag resolution against the
// shared schema table (slice 12), attribute-name resolution and the style name/number rules.

#include "node_lowering_internal.hpp"
#include "tessera/compiler/lowering/expression_lowering.hpp"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <system_error>
#include <utility>

namespace tessera::compiler::lowering::internal {
namespace {

bool tag_is_listed(std::span<const std::string_view> tags, std::string_view tag) {
    for (const std::string_view entry : tags) {
        if (entry == tag) {
            return true;
        }
    }
    return false;
}

const schema::PropRow* find_prop(std::span<const schema::PropRow> props, std::string_view name) {
    for (const schema::PropRow& prop : props) {
        if (prop.name == name) {
            return &prop;
        }
    }
    return nullptr;
}

// `on` followed by an uppercase letter is the event-prop shape of subset 10.6.
bool is_event_name(std::string_view name) {
    return name.size() > 2 && name[0] == 'o' && name[1] == 'n' && name[2] >= 'A' && name[2] <= 'Z';
}

std::optional<ir::NodeKind> ir_node_kind(std::string_view ir_kind) {
    if (ir_kind == "View") {
        return ir::NodeKind::View;
    }
    if (ir_kind == "Text") {
        return ir::NodeKind::Text;
    }
    if (ir_kind == "Button") {
        return ir::NodeKind::Button;
    }
    if (ir_kind == "TextInput") {
        return ir::NodeKind::TextInput;
    }
    if (ir_kind == "TextArea") {
        return ir::NodeKind::TextArea;
    }
    if (ir_kind == "Image") {
        return ir::NodeKind::Image;
    }
    if (ir_kind == "Path") {
        return ir::NodeKind::Path;
    }
    if (ir_kind == "Scroll") {
        return ir::NodeKind::Scroll;
    }
    if (ir_kind == "Spacer") {
        return ir::NodeKind::Spacer;
    }
    if (ir_kind == "VirtualList") {
        return ir::NodeKind::VirtualList;
    }
    if (ir_kind == "VirtualGrid") {
        return ir::NodeKind::VirtualGrid;
    }
    if (ir_kind == "ErrorBoundary") {
        return ir::NodeKind::ErrorBoundary;
    }
    if (ir_kind == "CustomElement") {
        return ir::NodeKind::CustomElement;
    }
    return std::nullopt;
}

// Section 3.4: a `Children` value has no expression form, so a `children` reference outside the
// dedicated `{children}` child position is rejected with the slot diagnostic (the slot itself is
// lowered before expression lowering sees it).
bool mentions_children_prop(const ir::Expr& expr) {
    if (expr.kind == ir::ExprKind::Ref) {
        return expr.id == "prop::children" || expr.id.rfind("prop::children.", 0) == 0;
    }
    for (const ir::ExprPtr& operand : expr.operands) {
        if (operand && mentions_children_prop(*operand)) {
            return true;
        }
    }
    for (const ir::TemplatePart& part : expr.parts) {
        if (part.expr && mentions_children_prop(*part.expr)) {
            return true;
        }
    }
    return false;
}

} // namespace

void fail_node(NodeState& state, std::string_view code, std::string_view message, jsx::Span span,
               std::string_view suggestion) {
    if (state.error) {
        return; // keep the first error; a later node does not replace it
    }
    state.error = jsx::Diagnostic{code,
                                  jsx::Severity::Error,
                                  std::string(message),
                                  state.context.source_map.resolve(span),
                                  std::string(suggestion),
                                  std::nullopt};
}

std::string next_node_id(NodeState& state) {
    return "node::" + std::to_string(state.next_node_id++);
}

ir::ExprPtr lower_node_expression(NodeState& state, const jsx::Expr& expr) {
    const ExpressionLoweringContext context{state.context.source_map, state.context.file,
                                            &state.names};
    ExpressionLoweringResult lowered = lower_expression(expr, context);
    if (!lowered) {
        if (!state.error && !lowered.error().empty()) {
            state.error = lowered.error().front();
        }
        if (!state.error) {
            fail_node(state, "TESSERA_IR_MALFORMED", "the expression produced no IR", expr.span);
        }
        return nullptr;
    }
    if (mentions_children_prop(**lowered)) {
        fail_node(state, "TESSERA_IR_SLOT",
                  "`children` is the slot; it has no value expression (ir-v1 section 3.4)",
                  expr.span, "write `{children}` as a child of an element");
        return nullptr;
    }
    return std::move(*lowered);
}

std::optional<ElementInfo> resolve_element(NodeState& state, const jsx::JsxNode& element) {
    if (element.kind != jsx::JsxKind::Element) {
        fail_node(state, "TESSERA_IR_MALFORMED", "only an element node has attributes and children",
                  element.span);
        return std::nullopt;
    }
    if (const schema::ElementRow* row = schema::find_element(element.tag); row != nullptr) {
        if (row->tag == "CustomElement") {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`<custom elements>` are lowered in slice 11", element.tag_span);
            return std::nullopt;
        }
        const std::optional<ir::NodeKind> kind = ir_node_kind(row->ir_kind);
        if (!kind) {
            fail_node(state, "TESSERA_IR_MALFORMED",
                      "the schema row of `" + std::string(row->tag) + "` has no IR node kind",
                      element.tag_span);
            return std::nullopt;
        }
        return ElementInfo{row, *kind, false, {}};
    }
    if (const schema::AliasRow* alias = schema::find_alias(element.tag); alias != nullptr) {
        if (alias->status == schema::AliasStatus::Unsupported) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`<" + std::string(element.tag) + ">` has no V1 intrinsic mapping",
                      element.tag_span, alias->alternative);
        } else {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "`<" + std::string(element.tag) + ">` is a lowercase alias of `<" +
                          std::string(alias->target) +
                          ">`; alias resolution is slice 13, write the intrinsic tag",
                      element.tag_span, "write `<" + std::string(alias->target) + ">`");
        }
        return std::nullopt;
    }
    for (const ComponentRef& reference : state.context.component_refs) {
        if (reference.name == element.tag) {
            ElementInfo info;
            info.kind = ir::NodeKind::Component;
            info.is_component = true;
            info.component_id = reference.id;
            return info;
        }
    }
    const bool component = !element.tag.empty() && element.tag[0] >= 'A' && element.tag[0] <= 'Z';
    if (component) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "`<" + element.tag +
                      ">` is neither an intrinsic element nor a component declared or imported in "
                      "this file (slice 9)",
                  element.tag_span, "declare or import the component, or fix the tag");
    } else {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "`<" + element.tag + ">` is not an intrinsic element", element.tag_span,
                  "use an intrinsic element of jsx-intrinsics-v1");
    }
    return std::nullopt;
}

AttributeInfo resolve_attribute(const ElementInfo& element, std::string_view name) {
    for (const schema::EventRow& event : element.row->events) {
        if (event.name == name) {
            return AttributeInfo{AttributeInfo::Kind::Event, &event, nullptr};
        }
    }
    if (name == "key") {
        return AttributeInfo{AttributeInfo::Kind::Key, nullptr, nullptr};
    }
    if (name == "ref") {
        return AttributeInfo{AttributeInfo::Kind::Ref, nullptr, nullptr};
    }
    if (name == "className") {
        return AttributeInfo{AttributeInfo::Kind::ClassName, nullptr, nullptr};
    }
    if (name == "style") {
        return AttributeInfo{AttributeInfo::Kind::Style, nullptr, nullptr};
    }
    if (name == "automationId") {
        return AttributeInfo{AttributeInfo::Kind::AutomationId, nullptr, nullptr};
    }
    if (const schema::PropRow* prop = find_prop(schema::common_props(), name); prop != nullptr) {
        if (tag_is_listed(prop->not_on, element.row->tag)) {
            return AttributeInfo{AttributeInfo::Kind::UnknownProp, nullptr, nullptr};
        }
        return AttributeInfo{AttributeInfo::Kind::CommonProp, nullptr, prop};
    }
    if (const schema::PropRow* prop = find_prop(element.row->props, name); prop != nullptr) {
        if (!prop->only_on.empty() && !tag_is_listed(prop->only_on, element.row->tag)) {
            return AttributeInfo{AttributeInfo::Kind::UnknownProp, nullptr, nullptr};
        }
        return AttributeInfo{AttributeInfo::Kind::ElementProp, nullptr, prop};
    }
    if (is_event_name(name)) {
        return AttributeInfo{AttributeInfo::Kind::UnknownEvent, nullptr, nullptr};
    }
    return AttributeInfo{AttributeInfo::Kind::UnknownProp, nullptr, nullptr};
}

std::string style_property_name(std::string_view camel_case) {
    std::string name;
    name.reserve(camel_case.size());
    for (const char c : camel_case) {
        if (c >= 'A' && c <= 'Z') {
            name.push_back('-');
            name.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            name.push_back(c);
        }
    }
    return name;
}

std::optional<std::string> px_text(double value) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    char buffer[64];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof buffer, value);
    if (result.ec != std::errc()) {
        return std::nullopt;
    }
    return std::string(buffer, result.ptr);
}

} // namespace tessera::compiler::lowering::internal
