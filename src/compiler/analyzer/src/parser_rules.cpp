// SPDX-License-Identifier: MIT

// Rules that can be decided from syntax alone while parsing: the list-loop form
// (docs/specs/jsx-subset-v1.md section 10.5), array methods (section 12), and the attribute rules
// of section 10.2 and 10.3 that need no component or type information. Rules that need
// declarations (event handler types, `ref` targets, where `key` is allowed) belong to the analyzer.

#include "parser_internal.hpp"

namespace tessera::compiler::jsx {
namespace {

bool is_rejected_array_method(std::string_view name) {
    static constexpr std::string_view kMethods[] = {
        "filter", "reduce", "reduceRight", "forEach", "slice",    "splice", "sort",    "reverse",
        "concat", "find",   "findIndex",   "some",    "every",    "flat",   "flatMap", "push",
        "pop",    "shift",  "unshift",     "indexOf", "includes", "join",   "fill",    "at",
    };
    for (const auto method : kMethods) {
        if (name == method) {
            return true;
        }
    }
    return false;
}

bool has_key_attribute(const JsxNode& element) {
    for (const auto& attribute : element.attributes) {
        if (attribute.name == "key" && attribute.kind != JsxAttrKind::Flag) {
            return true;
        }
    }
    return false;
}

} // namespace

// `call` is a freshly built Call whose callee may be `receiver.method`.
void Parser::check_call_rules(const Expr& call) {
    if (call.operands.empty() || call.operands[0]->kind != ExprKind::Member) {
        return;
    }
    const Expr& callee = *call.operands[0];
    const std::string& method = callee.text;
    if (is_rejected_array_method(method)) {
        report("TESSERA_JSX_SUBSET_ARRAY_METHOD",
               "array method `." + method + "` is not supported; only `.map` is", call.span,
               "filter or sort in a state setter or in developer C++");
        return;
    }
    if (method != "map") {
        return;
    }
    const auto shape_error = [&](const std::string& why) {
        report("TESSERA_JSX_SUBSET_CALL", "`.map` " + why, call.span,
               "write `list.map((item, index) => (<Row key={item.id} />))`");
    };
    const bool one_callback =
        call.operands.size() == 2 && call.operands[1]->kind == ExprKind::Arrow;
    if (!one_callback) {
        shape_error("takes exactly one arrow function");
        return;
    }
    const Expr& arrow = *call.operands[1];
    if (arrow.params.empty() || arrow.params.size() > 2) {
        shape_error("callback takes one or two parameters (item, optional index)");
        return;
    }
    const bool element_body = !arrow.arrow_block_body && arrow.operands[0]->kind == ExprKind::Jsx &&
                              arrow.operands[0]->jsx->kind == JsxKind::Element;
    if (!element_body) {
        shape_error("callback body must be a single JSX element, not a block or another value");
        return;
    }
    if (!has_key_attribute(*arrow.operands[0]->jsx)) {
        report("TESSERA_REACT_LIST_KEY", "the element returned by `.map` needs a `key` attribute",
               arrow.operands[0]->span, "add `key={item.id}`");
    }
}

void Parser::check_attribute_rules(const JsxAttribute& attribute) {
    if (attribute.kind == JsxAttrKind::Spread) {
        // Spread is allowed for the component's props object or an object literal, so the keys
        // are known at compile time (spec 10.2). Outside a component there is no props name.
        if (!in_component_) {
            return;
        }
        const Expr& source = *attribute.expr;
        const bool props = source.kind == ExprKind::Identifier && source.text == props_name_;
        if (!props && source.kind != ExprKind::Object) {
            report("TESSERA_JSX_SUBSET_SPREAD",
                   "spread is only allowed for the props parameter or an object literal",
                   attribute.span, "spread `props` or write the attributes out");
        }
        return;
    }
    if (attribute.name != "style") {
        return;
    }
    const auto dynamic = [&](const std::string& why) {
        report("TESSERA_JSX_SUBSET_STYLE_DYNAMIC", "`style` " + why, attribute.span,
               "write an inline object literal with identifier keys, for example "
               "`style={{ width: 10 }}`");
    };
    if (attribute.kind != JsxAttrKind::Expression || attribute.expr->kind != ExprKind::Object) {
        dynamic("must be an inline object literal");
        return;
    }
    for (const auto& property : attribute.expr->properties) {
        if (property.key_is_string) {
            dynamic("keys must be identifiers in camelCase, not strings");
            return;
        }
    }
}

} // namespace tessera::compiler::jsx
