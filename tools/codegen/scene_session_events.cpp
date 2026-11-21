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

void PreviewSession::bind_handler(std::uint64_t automation_id, const std::string& event,
                                  const std::string& handler_id) {
    auto slot = std::make_unique<HandlerSlot>();
    slot->handler_id = handler_id;

    // Resolve the IR body: a component handler (`handler::N`), or a native binding (`bind::X`)
    // whose `preview` block is the body. This mirrors `find_body` in
    // tests/parity/parity_runner.cpp, including the two cases it also refuses to run.
    for (const ir::Handler& handler : component_.handlers) {
        if (handler.id == handler_id) {
            slot->params = &handler.params;
            slot->actions = &handler.actions;
            break;
        }
    }
    if (slot->actions == nullptr) {
        for (const ir::Binding& binding : module_.bindings) {
            if (binding.id != handler_id ||
                (binding.component != component_.name && binding.component != component_.id)) {
                continue;
            }
            slot->params = &binding.params;
            // A native binding with no preview body does nothing in the preview, exactly as in the
            // parity runner: the developer's C++ is the thing this slice does not run.
            slot->actions = binding.preview ? &binding.preview->actions : &kNoActions;
            break;
        }
    }
    if (slot->actions == nullptr) {
        slot->reason = "no handler or native binding with that id in `" + component_.name + "`";
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
