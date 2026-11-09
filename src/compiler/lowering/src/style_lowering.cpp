// SPDX-License-Identifier: MIT

// Inline `style` objects (docs/specs/jsx-subset-v1.md section 10.3, docs/design/jsx-ir-lowering.md
// decision 10): camelCase keys become kebab-case names, a number is "<n>px" for a length property
// and stays unitless otherwise, and every name is checked against the shared style table.

#include "node_lowering_internal.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::lowering::internal {

bool lower_style(NodeState& state, const jsx::Expr& object, ir::Node& node) {
    if (object.kind != jsx::ExprKind::Object) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "`style` must be an inline object literal (subset 10.3)", object.span);
        return false;
    }
    for (const jsx::ObjectProperty& property : object.properties) {
        if (!property.value) {
            fail_node(state, "TESSERA_IR_MALFORMED", "a style property has no value",
                      property.key_span);
            return false;
        }
        const std::string name = style_property_name(property.key);
        const schema::StyleProperty* style_property = schema::find_style_property(name);
        if (style_property == nullptr) {
            fail_node(state, "TESSERA_IR_UNKNOWN_PROP",
                      "`" + property.key + "` is not a supported style property", property.key_span,
                      "see the property list of stylesheet-v1");
            return false;
        }
        const jsx::Expr& raw = *property.value;
        ir::ExprPtr value;
        if (raw.kind == jsx::ExprKind::Number &&
            style_property->kind == schema::StyleKind::Length) {
            const std::optional<std::string> text = px_text(raw.number);
            if (!text) {
                fail_node(state, "TESSERA_IR_LITERAL", "the style number is not finite", raw.span);
                return false;
            }
            value = ir::make_literal(*text + "px");
        } else {
            value = lower_node_expression(state, raw);
        }
        if (!value) {
            return false;
        }
        node.style[name] = std::move(value);
    }
    return true;
}

} // namespace tessera::compiler::lowering::internal
