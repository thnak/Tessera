// SPDX-License-Identifier: MIT
//
// The preview session's scene-event bindings (issue #168, P4): which IR handler a scene node's
// event must run, and the callback that marks it reached. Split out of `scene_session.cpp`, which
// is at its size budget.
//
// The preview is an IR evaluator: it does not run the developer's C++. A click goes through the
// engine's own `ui::InputRouter`, and what that path reaches is a `Node::bind_event` callback this
// file installs; the callback runs the IR *preview body* of the node's handler through
// `eval::Evaluator::run`.

#include "scene_build.hpp"
#include "scene_session.hpp"
#include "scene_session_slots.hpp"

#include <memory>
#include <string>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

// A native binding with no preview body installs no actions. A static empty vector keeps the slot's
// "resolved, but nothing to run" state distinct from "not resolved at all", which is what decides
// whether a dispatch reports the handler as run or as unrun.
const std::vector<ir::Action> kNoActions{};

// The scene event type an IR event name maps to, for the events this slice binds. A click bubbles
// through bindings (event.cpp), so binding Click is what a scripted click needs.
constexpr ui::EventType* click_type() {
    static ui::EventType type = ui::EventType::Click;
    return &type;
}

} // namespace

// The IR body of `handler_id` in `component`: a component handler (`handler::N`), or a native
// binding (`bind::X`) whose `preview` block is the body. False when neither exists. Shared with
// `scene_session_text.cpp` (declared in `scene_session_slots.hpp`).
bool resolve_body(const ir::IrModule& module, const ir::Component& component,
                  const std::string& handler_id, const std::vector<ir::Param>*& params,
                  const std::vector<ir::Action>*& actions) {
    for (const ir::Handler& handler : component.handlers) {
        if (handler.id == handler_id) {
            params = &handler.params;
            actions = &handler.actions;
            return true;
        }
    }
    for (const ir::Binding& binding : module.bindings) {
        if (binding.id != handler_id ||
            (binding.component != component.name && binding.component != component.id)) {
            continue;
        }
        params = &binding.params;
        actions = binding.preview ? &binding.preview->actions : &kNoActions;
        return true;
    }
    return false;
}

// The state of a callback chain: the frame whose handler is running, and the one that called it.
struct PreviewSession::CallContext {
    PreviewSession* session = nullptr;
    std::size_t frame = 0;
    CallContext* outer = nullptr;
};

bool PreviewSession::run_actions(std::size_t frame, const std::vector<ir::Action>& actions,
                                 eval::Scope* scope) {
    CallContext context{this, frame, active_call_};
    active_call_ = &context;
    evaluator_.set_prop_call_sink(&PreviewSession::on_prop_call, &context);
    const bool ok =
        evaluator_.run(actions, scope != nullptr ? *scope : *frames_[frame].scope).has_value();
    active_call_ = context.outer;
    evaluator_.set_prop_call_sink(
        context.outer != nullptr ? &PreviewSession::on_prop_call : nullptr, context.outer);
    return ok;
}

// A `callProp` in a callee's handler: the caller's handler bound to that prop runs against the
// caller's scope. A payload callback needs a record value the evaluator cannot build, so it is
// reported rather than run, like a payload-taking handler.
void PreviewSession::on_prop_call(void* context, const std::string& target, bool has_payload) {
    auto* const call = static_cast<CallContext*>(context);
    PreviewSession& self = *call->session;
    const SceneInstanceFrame& frame = self.frames_[call->frame];
    constexpr std::string_view kPrefix = "prop::";
    const std::string prop = target.rfind(kPrefix, 0) == 0 ? target.substr(kPrefix.size()) : target;
    const auto bound = frame.callbacks.find(prop);
    if (bound == frame.callbacks.end()) {
        return; // an unset callback is a no-op
    }
    const std::string& handler_id = bound->second;
    const std::vector<ir::Param>* params = nullptr;
    const std::vector<ir::Action>* actions = nullptr;
    const SceneInstanceFrame& caller = self.frames_[frame.parent];
    if (!resolve_body(self.module_, *caller.component, handler_id, params, actions)) {
        self.unrun_.push_back(handler_id);
        return;
    }
    if (has_payload || (params != nullptr && !params->empty())) {
        self.unrun_.push_back(handler_id);
        return;
    }
    if (self.run_actions(frame.parent, *actions)) {
        self.ran_.push_back(handler_id);
        self.dirty_ = true;
    } else {
        self.unrun_.push_back(handler_id);
    }
}

void PreviewSession::bind_handler(std::uint64_t automation_id, const std::string& event,
                                  const std::string& handler_id, std::size_t frame) {
    auto slot = std::make_unique<HandlerSlot>();
    slot->handler_id = handler_id;
    slot->frame = frame;
    const ir::Component& owner = *frames_[frame].component;

    (void)resolve_body(module_, owner, handler_id, slot->params, slot->actions);
    if (slot->actions == nullptr) {
        slot->reason = "no handler or native binding with that id in `" + owner.name + "`";
    } else if (slot->params != nullptr && !slot->params->empty()) {
        // A payload-taking handler needs a record value (`ClickEvent`, `KeyEvent`) bound to its
        // parameter, which the evaluator cannot construct: the preview dispatches the event but
        // cannot run this body. Reported, never silently treated as "nothing happened".
        slot->reason = "the handler takes a payload parameter (`" + slot->params->front().name +
                       "`), which the evaluator has no record value for";
    } else {
        slot->can_run = true;
    }

    HandlerSlot* const raw = slot.get();
    slots_.push_back(std::move(slot));

    if (event != "onClick") {
        return; // an IR event with no scene counterpart in this slice
    }
    if (ui::Node* node = node_by_automation_id(automation_id)) {
        (void)node->bind_event(*click_type(), &PreviewSession::on_scene_event, raw);
    }
}

void PreviewSession::on_scene_event(void* context, const ui::Event& event) {
    auto* const slot = static_cast<HandlerSlot*>(context);
    if (slot == nullptr || event.type != ui::EventType::Click) {
        return;
    }
    // The binding only reaches a handler that exists; the session runs it from the post-dispatch
    // pass, because a body that mutates state must not rebuild the tree it is being dispatched
    // through. Marking the slot is enough: the router visits ancestors in order, and `dispatch()`
    // runs every marked slot in binding order afterwards.
    slot->reached = true;
}

} // namespace tessera::compiler::codegen::cli
