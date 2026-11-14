// SPDX-License-Identifier: MIT

// Event attributes of slice 7 (docs/specs/jsx-subset-v1.md section 9). The value is a declared
// handler (`bind::` or `handler::` id), an inline setter arrow (a `handler::N` record) or a state
// setter reference (a `handler::N` record with one `set` action); the payload record of the event
// comes from the shared schema table.

#include "node_lowering_internal.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {
namespace {

std::string event_name(const schema::EventRow& event) {
    return std::string(event.name);
}

// A setter reference (subset 9.1) receives the primary member of the payload record when it has
// one, and the whole payload otherwise.
void setter_action_value(const schema::TypeRecord* record, ir::Action& action) {
    if (record != nullptr && !record->primary.empty()) {
        action.value = ir::make_ref("e." + std::string(record->primary));
    } else {
        action.value = ir::make_ref("e");
    }
}

bool lower_event_arrow(NodeState& state, const schema::EventRow& event, const jsx::Expr& arrow,
                       jsx::Span span, ir::Node& node) {
    if (arrow.params.size() > 1) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "an event handler takes at most one parameter (subset 9.4)", span);
        return false;
    }
    std::vector<ir::Param> params;
    if (!arrow.params.empty()) {
        if (event.payload.empty()) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "the payload of `" + event_name(event) +
                          "` is defined by the manifest, not by the schema",
                      span);
            return false;
        }
        params.push_back(ir::Param{arrow.params[0], std::string(event.payload)});
    }
    HandlerActionsResult actions = lower_handler_actions(arrow, state.context);
    if (!actions) {
        if (!state.error && !actions.error().empty()) {
            state.error = actions.error().front();
        }
        return false;
    }
    if (const std::optional<jsx::Diagnostic> violation =
            check_callback_calls(state.context, params, *actions, arrow.span)) {
        state.error = violation;
        return false;
    }
    const std::string id = state.hooks.add_handler(
        std::move(params), std::move(*actions),
        to_source_span(state.context.source_map, arrow.span, state.context.file));
    node.events[event_name(event)] = id;
    return true;
}

bool lower_setter_reference(NodeState& state, const schema::EventRow& event,
                            const NamedSetter& setter, jsx::Span span, ir::Node& node) {
    if (event.payload.empty()) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "`" + event_name(event) + "` has no payload record to pass to a setter", span);
        return false;
    }
    const schema::TypeRecord* record = schema::find_type_record(event.payload);
    if (record == nullptr) {
        fail_node(state, "TESSERA_IR_MALFORMED",
                  "the schema has no payload record `" + std::string(event.payload) + "`", span);
        return false;
    }
    ir::Action action;
    action.kind = ir::ActionKind::Set;
    action.target = setter.state_id;
    setter_action_value(record, action);
    std::vector<ir::Action> actions;
    actions.push_back(std::move(action));
    std::vector<ir::Param> params;
    params.push_back(ir::Param{"e", std::string(event.payload)});
    const std::string id =
        state.hooks.add_handler(std::move(params), std::move(actions),
                                to_source_span(state.context.source_map, span, state.context.file));
    node.events[event_name(event)] = id;
    return true;
}

} // namespace

bool lower_event_value(NodeState& state, const ElementInfo& info, const schema::EventRow& event,
                       const jsx::Expr& value, jsx::Span span, ir::Node& node) {
    if (value.kind == jsx::ExprKind::Arrow) {
        return lower_event_arrow(state, event, value, span, node);
    }
    if (value.kind == jsx::ExprKind::Identifier) {
        if (const HandlerRef* handler = state.hooks.find_handler(value.text); handler != nullptr) {
            node.events[event_name(event)] = handler->id;
            return true;
        }
        if (const NamedSetter* setter = state.hooks.find_setter(value.text); setter != nullptr) {
            return lower_setter_reference(state, event, *setter, span, node);
        }
        const ForwardedCallback forwarded =
            forward_callback_prop(state, value.text, event_name(event), event.payload, span);
        if (forwarded.matched) {
            if (forwarded.handler_id.empty()) {
                return false;
            }
            node.events[event_name(event)] = forwarded.handler_id;
            return true;
        }
        fail_node(state, "TESSERA_IR_UNRESOLVED",
                  "`" + value.text +
                      "` is not a handler, a state setter or a callback prop of "
                      "this component",
                  span);
        return false;
    }
    (void)info;
    fail_node(state, "TESSERA_IR_UNSUPPORTED", "an event value must be a handler (subset 9)", span);
    return false;
}

} // namespace tessera::compiler::lowering::internal
