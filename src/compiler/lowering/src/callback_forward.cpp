// SPDX-License-Identifier: MIT

// Forwarding a callback prop of the component to an event (docs/specs/jsx-subset-v1.md section 9.5,
// issue #286): `onClick={onPress}` where `onPress` is a `() => void` or `(e: Record) => void` prop
// of the enclosing component. The event runs one `callProp` action; nothing is captured and no
// closure object exists at runtime, the call goes through the prop's `std::function` member.

#include "node_lowering_internal.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::lowering::internal {

ForwardedCallback forward_callback_prop(NodeState& state, std::string_view name,
                                        std::string_view event_name, std::string_view payload,
                                        jsx::Span span) {
    ForwardedCallback result;
    const rules::Symbol* symbol = state.context.analysis.find(name);
    if (symbol == nullptr || symbol->kind != rules::SymbolKind::Prop) {
        return result;
    }
    result.matched = true;
    const ir::Prop* prop = nullptr;
    if (state.declared_props != nullptr) {
        for (const ir::Prop& candidate : *state.declared_props) {
            if (candidate.name == name) {
                prop = &candidate;
            }
        }
    }
    if (prop == nullptr) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "the type of prop `" + std::string(name) +
                      "` is not known, so it cannot be forwarded as the handler of `" +
                      std::string(event_name) + "`",
                  span,
                  "declare it in the component's props type: `" + std::string(name) +
                      ": () => void`");
        return result;
    }
    const bool payload_free = prop->type == "callback";
    if (!payload_free && (prop->type == "children" || prop->type.rfind("optional<", 0) == 0 ||
                          schema::find_type_record(prop->type) == nullptr)) {
        fail_node(state, "TESSERA_IR_UNSUPPORTED",
                  "prop `" + std::string(name) +
                      "` is not a callback and cannot be the handler of `" +
                      std::string(event_name) + "`",
                  span, "declare it as `() => void` or `(e: Record) => void`");
        return result;
    }
    std::vector<ir::Param> params;
    ir::Action action;
    action.kind = ir::ActionKind::CallProp;
    action.target = "prop::" + prop->name;
    if (!payload_free) {
        // A payload callback receives the event's payload unchanged, so the record must be the
        // one the event delivers.
        if (payload.empty() || prop->type != payload) {
            fail_node(state, "TESSERA_IR_UNSUPPORTED",
                      "callback prop `" + std::string(name) + "` takes `" + prop->type + "` but `" +
                          std::string(event_name) + "` delivers " +
                          (payload.empty() ? std::string("no payload record")
                                           : "`" + std::string(payload) + "`"),
                      span);
            return result;
        }
        params.push_back(ir::Param{"e", std::string(payload)});
        action.value = ir::make_ref("e");
    }
    std::vector<ir::Action> actions;
    actions.push_back(std::move(action));
    result.handler_id =
        state.hooks.add_handler(std::move(params), std::move(actions),
                                to_source_span(state.context.source_map, span, state.context.file));
    return result;
}

} // namespace tessera::compiler::lowering::internal
