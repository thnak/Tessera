// SPDX-License-Identifier: MIT
//
// `onSizeChange` in the preview session (ADR-041 item 3, issue #694; the contract is
// docs/specs/size-change-events-v1.md). Split out of `scene_session.cpp`, which is at its size
// budget.
//
// The preview is an IR evaluator: it does not run the developer's C++ thunk. It runs the same IR
// body the generated base's thunk would call, with a `SizeChangeEvent` record bound to the
// handler's parameter, so a previewed component reacts to its own size exactly as the native build
// does.

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
// "resolved, but nothing to run" state distinct from "not resolved at all".
const std::vector<ir::Action> kNoActions{};

} // namespace

void PreviewSession::attach_size_observations(const SceneBuildResult& built) {
    for (const SceneBuildResult::SceneEventHandler& handler : built.handlers) {
        if (handler.event != "onSizeChange") {
            continue;
        }
        ui::Node* const node = node_by_automation_id(handler.automation_id);
        if (node == nullptr) {
            continue;
        }
        auto slot = std::make_unique<SizeSlot>();
        slot->session = this;
        slot->handler_id = handler.handler;
        // The same resolution `bind_handler` does: a component handler (`handler::N`) or a native
        // binding's preview body. A native binding with no preview body does nothing here, exactly
        // as in the parity runner.
        for (const ir::Handler& candidate : component_.handlers) {
            if (candidate.id == handler.handler) {
                slot->params = &candidate.params;
                slot->actions = &candidate.actions;
                break;
            }
        }
        if (slot->actions == nullptr) {
            for (const ir::Binding& binding : module_.bindings) {
                if (binding.id != handler.handler ||
                    (binding.component != component_.name && binding.component != component_.id)) {
                    continue;
                }
                slot->params = &binding.params;
                slot->actions = binding.preview ? &binding.preview->actions : &kNoActions;
                break;
            }
        }
        if (slot->actions == nullptr) {
            slot->reason = "no handler or native binding with that id in `" + component_.name + "`";
        } else if (slot->params != nullptr && !slot->params->empty() &&
                   slot->params->front().type != "SizeChangeEvent") {
            // A payload the evaluator cannot build is reported, never silently treated as "nothing
            // happened": `onSizeChange` takes a `SizeChangeEvent` and nothing else.
            slot->reason = "the handler takes a `" + slot->params->front().type +
                           "` parameter, but `onSizeChange` delivers a `SizeChangeEvent`";
        } else {
            slot->can_run = true;
        }
        SizeSlot* const raw = slot.get();
        size_slots_.push_back(std::move(slot));
        // The observation is attached AFTER layout (this runs at the end of `rebuild()`), so the
        // first delivery carries the size the tree actually has rather than 0x0.
        raw->observation.emplace(*node, &PreviewSession::on_size_change, raw);
        settle_.observers().attach(*raw->observation);
    }
}

void PreviewSession::detach_size_observations() {
    for (const auto& slot : size_slots_) {
        if (slot->observation.has_value()) {
            settle_.observers().detach(*slot->observation);
        }
    }
    size_slots_.clear();
}

void PreviewSession::on_size_change(void* context, const ui::layout::SizeChange& size) {
    auto* const slot = static_cast<SizeSlot*>(context);
    if (slot == nullptr || slot->session == nullptr) {
        return;
    }
    if (!slot->can_run) {
        // Reported through the same `unrun` list a dispatch uses, so a page can show why a
        // previewed callback did nothing.
        slot->session->unrun_.push_back(slot->handler_id);
        return;
    }
    // The payload record the generated thunk builds: `SizeChangeEvent{width, height, contentWidth,
    // contentHeight}`. The evaluator resolves `e.width` as a dotted member read (ADR-051 slice 5),
    // so an inline `(e) => setWide(e.width >= 600)` runs here exactly as it does natively.
    eval::Record record;
    record.type = "SizeChangeEvent";
    record.fields.push_back({"width", eval::Value::of_float(size.width)});
    record.fields.push_back({"height", eval::Value::of_float(size.height)});
    record.fields.push_back({"contentWidth", eval::Value::of_float(size.content_width)});
    record.fields.push_back({"contentHeight", eval::Value::of_float(size.content_height)});
    // The body runs against the SESSION's scope, so a `setWide(...)` it performs is the state the
    // next rebuild reads. The parameter is bound in a copy that is merged back, exactly as the
    // parity runner does for a handler with arguments.
    eval::Scope with_params = slot->session->scope_;
    if (slot->params != nullptr && !slot->params->empty()) {
        with_params[slot->params->front().name] = eval::Value::of_record(std::move(record));
    }
    if (!slot->session->evaluator_.run(*slot->actions, with_params).has_value()) {
        slot->session->unrun_.push_back(slot->handler_id);
        return;
    }
    slot->session->ran_.push_back(slot->handler_id);
    for (auto& [name, value] : slot->session->scope_) {
        const auto found = with_params.find(name);
        if (found != with_params.end()) {
            value = std::move(found->second);
        }
    }
    // The scope changed, so the tree's text and boxes are stale. The rebuild is deferred to the
    // settle loop's relayout hook: a callback that rebuilds the tree it is being delivered through
    // would invalidate the observation the loop is walking.
    slot->session->dirty_ = true;
}

void PreviewSession::settle_relayout(void* context) {
    auto* self = static_cast<PreviewSession*>(context);
    // A settle round's relayout is the preview's re-evaluation: the scope is the source of truth
    // and the scene is derived from it, so a callback that set state needs `rebuild()`, not a
    // re-layout of the stale tree. `rebuild()` re-attaches the observations and re-runs the layout,
    // which is exactly what the loop's next round reads.
    self->dirty_ = false;
    self->rebuild();
}

} // namespace tessera::compiler::codegen::cli
