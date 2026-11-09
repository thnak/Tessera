// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/lowering/component_names.hpp"
#include "tessera/compiler/lowering/hook_lowering.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/lowering/source_span.hpp"
#include "tessera/schema/intrinsics.hpp"
#include "tessera/schema/style.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Internal seam of node lowering (slice 7). Not installed; only src/compiler/lowering includes it.
namespace tessera::compiler::lowering::internal {

struct NodeState {
    NodeState(const HookLoweringContext& lowering_context, LoweredHooks& lowered_hooks)
        : context(lowering_context),
          hooks(lowered_hooks),
          names(lowering_context.analysis) {}

    const HookLoweringContext& context;
    LoweredHooks& hooks;
    ComponentNames names;
    std::optional<jsx::Diagnostic> error; // the first error; later reports are ignored
    unsigned next_node_id = 1;            // `node::N`, pre-order per component (decision 2)
    unsigned loop_depth = 0;              // enclosing `.map` loops; at most 2 (ir-v1 section 5)
};

// Reports a TESSERA_IR_* error at `span` unless one was already reported. The diagnostic keeps a
// string_view code, so `code` must point at a string literal.
void fail_node(NodeState& state, std::string_view code, std::string_view message, jsx::Span span,
               std::string_view suggestion = {});

// The next `node::N` id.
[[nodiscard]] std::string next_node_id(NodeState& state);

// Lowers one pure expression with the component's names (slice 5 + the analysis symbol table).
[[nodiscard]] ir::ExprPtr lower_node_expression(NodeState& state, const jsx::Expr& expr);

// The intrinsic row and IR kind of a JSX element, or null after reporting why it cannot be lowered
// in this slice (component reference, custom element, HTML alias, unknown tag).
struct ElementInfo {
    const schema::ElementRow* row = nullptr;
    ir::NodeKind kind = ir::NodeKind::View;
    bool is_component = false; // slice 9: kind is Component and `component_id` is set
    std::string component_id;  // `comp::<Name>`
};
[[nodiscard]] std::optional<ElementInfo> resolve_element(NodeState& state,
                                                         const jsx::JsxNode& element);

// Slice 9: attributes and children of a `Component` node. Props are accepted by name (their types
// are checked by slice 4); callback props are events; children are the caller's slot content.
[[nodiscard]] bool lower_component_attributes(NodeState& state, const jsx::JsxNode& element,
                                              const ElementInfo& info, ir::Node& node);
[[nodiscard]] bool lower_slot_children(NodeState& state, const jsx::JsxNode& element,
                                       ir::Node& node);

// What an attribute name means on one element, from the shared schema table.
struct AttributeInfo {
    enum class Kind : std::uint8_t {
        Event,
        Key,
        Ref,
        ClassName,
        Style,
        AutomationId,
        CommonProp,
        ElementProp,
        UnknownEvent,
        UnknownProp,
    };

    Kind kind = Kind::UnknownProp;
    const schema::EventRow* event = nullptr;
    const schema::PropRow* prop = nullptr; // CommonProp and ElementProp
};
[[nodiscard]] AttributeInfo resolve_attribute(const ElementInfo& element, std::string_view name);

// Builds one element (and its subtree) with the next `node::N` id. Null after reporting an error.
[[nodiscard]] std::optional<ir::Node> make_node(NodeState& state, const jsx::JsxNode& element);

// Attributes and children of an element already created by make_node.
[[nodiscard]] bool lower_attributes(NodeState& state, const jsx::JsxNode& element,
                                    const ElementInfo& info, ir::Node& node);
[[nodiscard]] bool lower_children(NodeState& state, const jsx::JsxNode& element,
                                  const ElementInfo& info, ir::Node& node);

// An event attribute value: a declared handler, a setter reference or an inline setter arrow
// (subset 9). Appends a `handler::N` record when the value is an arrow or a setter reference.
[[nodiscard]] bool lower_event_value(NodeState& state, const ElementInfo& info,
                                     const schema::EventRow& event, const jsx::Expr& value,
                                     jsx::Span span, ir::Node& node);

// `style={{...}}`: object literal only; camelCase keys become kebab-case and numbers of length
// properties become "<n>px" (decision 10). The property names are checked against the schema.
[[nodiscard]] bool lower_style(NodeState& state, const jsx::Expr& object, ir::Node& node);

// `backgroundColor` -> `background-color` (subset 10.3).
[[nodiscard]] std::string style_property_name(std::string_view camel_case);

// The shortest text of `value` for a px value ("14", "0.5"), or null when it is not finite.
[[nodiscard]] std::optional<std::string> px_text(double value);

// Slice 8: the two control forms that a JSX child can have (docs/specs/jsx-subset-v1.md sections
// 10.4 and 10.5). `None` means an ordinary expression or element child.
enum class ControlKind : std::uint8_t { None, Conditional, Loop };
[[nodiscard]] ControlKind classify_control(const jsx::Expr& expr);

// Lowers one conditional or loop child into one or more sibling nodes, in source order:
// - `c && <A/>`                 -> A with `condition` = c
// - `c ? <A/> : <B/>`           -> A with `condition` = c, then B with `condition` = !c
// - `c ? <A/> : null|false`     -> A with `condition` = c
// - `c ? null|false : <B/>`     -> B with `condition` = !c
// - `items.map(item => <A/>)`   -> A with `loop` and the `key` expression of the element.
// Node ids are assigned in source order for every branch whether or not it is taken at run time.
[[nodiscard]] bool lower_control_child(NodeState& state, const jsx::Expr& expr,
                                       std::vector<ir::Node>& nodes);

} // namespace tessera::compiler::lowering::internal
