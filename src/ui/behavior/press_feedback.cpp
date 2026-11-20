// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/press_feedback.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

namespace tessera::ui::behavior {
namespace {

// The scene's interaction state is the single source of truth: `InputRouter` already normalizes
// pointer, touch and keyboard input into `hovered()`/`active()`, and `FocusManager` owns keyboard
// focus. Reading them here is what keeps `press_feedback` free of its own listeners, and what makes
// a headless test with the manual clock deterministic.
struct InteractionState {
    bool pressed = false;
    bool hovered = false;
    bool focused = false;
};

InteractionState read_state(const Node* target, const InputRouter* router) {
    InteractionState state;
    if (target == nullptr || router == nullptr) {
        return state;
    }
    state.pressed = router->active() == target;
    state.hovered = router->hovered() == target;
    state.focused = router->focus_manager().focused() == target;
    return state;
}

} // namespace

void PressFeedback::attach(const Targets& targets, const Services& services, const ArgsView& args) {
    (void)args; // `press_feedback` has no options; the manifest declares none.
    if (!targets.all_attached() || targets.nodes.empty()) {
        return;
    }
    target_ = targets.nodes.front();
    router_ = services.router;
    refresh();
}

void PressFeedback::update(const ArgsView& args) {
    (void)args;
    // The options are empty, so an update can only mean the scene moved on; recompute rather than
    // assume the caller noticed.
    refresh();
}

void PressFeedback::detach() {
    target_ = nullptr;
    router_ = nullptr;
    // Before the first attach and after detach an output holds its manifest default, which for all
    // three is `false`.
    outputs_ = {};
}

std::span<const OutputValue> PressFeedback::outputs() const {
    return outputs_;
}

void PressFeedback::refresh() {
    const InteractionState state = read_state(target_, router_);
    outputs_[0].kind = OutputValue::Kind::Bool;
    outputs_[0].boolean = state.pressed;
    outputs_[1].kind = OutputValue::Kind::Bool;
    outputs_[1].boolean = state.hovered;
    outputs_[2].kind = OutputValue::Kind::Bool;
    outputs_[2].boolean = state.focused;
}

} // namespace tessera::ui::behavior
