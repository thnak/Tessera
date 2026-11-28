// SPDX-License-Identifier: MIT

// Alias attribute rewrites that carry a value to the scene (docs/specs/jsx-intrinsics-v1/
// html-aliases.md sections 3.5 and 3.7): `ol start`/`reversed`, `li value`, `a href`,
// `label htmlFor`, and the `a target`/`rel` values that are accepted and dropped. Split from
// node_attributes.cpp, which keeps the schema-driven props and events.

#include "node_lowering_internal.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::lowering::internal {
namespace {

void put_prop(ir::Node& node, std::string_view name, ir::ExprPtr value) {
    node.props[std::string(name)] = std::move(value);
}

// A static integer: `3` or `-3`.
std::optional<std::int64_t> static_integer(const jsx::Expr& value) {
    const jsx::Expr* number = &value;
    bool negative = false;
    if (value.kind == jsx::ExprKind::Unary && value.text == "-" && value.operands.size() == 1 &&
        value.operands[0]) {
        number = value.operands[0].get();
        negative = true;
    }
    if (number->kind != jsx::ExprKind::Number || number->number != std::floor(number->number) ||
        std::fabs(number->number) > 2147483647.0) {
        return std::nullopt;
    }
    const auto integer = static_cast<std::int64_t>(number->number);
    return negative ? -integer : integer;
}

// A ValueMap attribute whose accepted values all map to nothing (`a target="_blank"`, `a rel`):
// the literal is checked and dropped, anything else is the alias row's rejection.
bool drop_alias_value(NodeState& state, const ElementInfo& element,
                      const schema::AliasAttr& rewrite, const jsx::JsxAttribute& attribute) {
    std::optional<std::string> text;
    if (attribute.kind == jsx::JsxAttrKind::String) {
        text = attribute.string_value;
    } else if (attribute.kind == jsx::JsxAttrKind::Expression && attribute.expr &&
               attribute.expr->kind == jsx::ExprKind::String) {
        text = attribute.expr->text;
    }
    if (text) {
        for (const schema::AliasValue& accepted : rewrite.values) {
            if (!accepted.rejected && accepted.value == *text && accepted.replacement.empty()) {
                return true;
            }
        }
    }
    fail_node(state, "TESSERA_JSX_SUBSET_HTML_ATTR",
              "`" + std::string(attribute.name) + "` on `<" + std::string(element.alias_tag) +
                  ">` has no V1 meaning for this value",
              attribute.span, std::string(rewrite.suggestion));
    return false;
}

// `a href` and `label htmlFor` (html-aliases.md section 3.5, issue #542): a static string carried
// as an `aria-*` pair the scene reads at activation (`htmlFor` may also be an expression). The pair
// table truncates at `kMaxAriaValueBytes`, and a silently shortened URL would go somewhere else, so
// a longer value is refused here. A link is also a Tab stop unless the author already chose its
// tabIndex.
bool lower_link_reference(NodeState& state, const ElementInfo& element, std::string_view target,
                          const jsx::JsxAttribute& attribute, ir::Node& node) {
    std::optional<std::string> text;
    if (attribute.kind == jsx::JsxAttrKind::String) {
        text = attribute.string_value;
    } else if (attribute.kind == jsx::JsxAttrKind::Expression && attribute.expr &&
               attribute.expr->kind == jsx::ExprKind::String) {
        text = attribute.expr->text;
    }
    // `htmlFor={id}` is the common React shape (a prop or `useId()`), and an id is short, so it may
    // be an expression like `aria-labelledby`. A URL may not: a truncated one would go elsewhere.
    if (!text && target == "aria-for" && attribute.kind == jsx::JsxAttrKind::Expression &&
        attribute.expr) {
        ir::ExprPtr lowered = lower_node_expression(state, *attribute.expr);
        if (!lowered) {
            return false;
        }
        put_prop(node, target, std::move(lowered));
        return true;
    }
    constexpr std::size_t kMaxBytes = 128; // ui::kMaxAriaValueBytes
    if (!text || text->empty() || text->size() > kMaxBytes) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "`" + std::string(attribute.name) + "` on `<" + std::string(element.alias_tag) +
                      ">` takes a non-empty string literal of at most 128 bytes",
                  attribute.span,
                  target == "aria-href" ? "write an absolute route path (`/settings`) or a URL"
                                        : "write the id of the control");
        return false;
    }
    put_prop(node, target, ir::make_literal(std::move(*text)));
    if (target == "aria-href" && node.props.find("tabIndex") == node.props.end()) {
        put_prop(node, "tabIndex", ir::make_literal(std::int64_t{0}));
    }
    return true;
}

} // namespace

// `ol start`, `ol reversed` and `li value` (html-aliases.md section 3.7): the alias row renames
// them to `aria-*` pairs the marker painter reads. Only a rename to an `aria-*` name is applied
// here (the other renames are still spec text); the value must be static, as the pair is emitted
// once. Returns nullopt when the attribute is not such a rewrite, true or false when it was
// handled.
std::optional<bool> lower_alias_attribute(NodeState& state, const ElementInfo& element,
                                          const jsx::JsxAttribute& attribute, ir::Node& node) {
    if (element.alias_tag.empty()) {
        return std::nullopt;
    }
    const schema::AliasRow* alias = schema::find_alias(element.alias_tag);
    if (alias == nullptr) {
        return std::nullopt;
    }
    for (const schema::AliasAttr& rewrite : alias->attrs) {
        if (rewrite.name == attribute.name && rewrite.kind == schema::AliasAttrKind::ValueMap &&
            element.alias_tag == "a") {
            return drop_alias_value(state, element, rewrite, attribute);
        }
        if (rewrite.name != attribute.name || rewrite.kind != schema::AliasAttrKind::Rename ||
            rewrite.rename.rfind("aria-", 0) != 0) {
            continue;
        }
        if (rewrite.rename == "aria-href" || rewrite.rename == "aria-for") {
            return lower_link_reference(state, element, rewrite.rename, attribute, node);
        }
        std::optional<std::string> text;
        const bool boolean_target = rewrite.rename == "aria-reversed";
        if (attribute.kind == jsx::JsxAttrKind::Flag) {
            text = "true";
        } else if (attribute.kind == jsx::JsxAttrKind::String) {
            text = attribute.string_value;
        } else if (attribute.kind == jsx::JsxAttrKind::Expression && attribute.expr) {
            if (boolean_target && attribute.expr->kind == jsx::ExprKind::Boolean) {
                text = attribute.expr->boolean ? "true" : "false";
            } else if (const auto integer = static_integer(*attribute.expr); integer) {
                text = std::to_string(*integer);
            }
        }
        if (!text || (boolean_target && *text != "true" && *text != "false")) {
            fail_node(
                state, "TESSERA_IR_UNSUPPORTED",
                "`" + std::string(attribute.name) + "` on `<" + std::string(element.alias_tag) +
                    ">` takes " +
                    (boolean_target ? "a bare flag or a literal boolean" : "a literal integer"),
                attribute.span);
            return false;
        }
        put_prop(node, rewrite.rename, ir::make_literal(std::move(*text)));
        return true;
    }
    return std::nullopt;
}

} // namespace tessera::compiler::lowering::internal
