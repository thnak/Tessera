// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/press_feedback.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

namespace tessera::ui::behavior {
namespace {

// The manifest's option order: `disabled`.
constexpr std::string_view kOptionNames[] = {"disabled"};

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

// The `disabled` option, which suppresses `pressed` only. A disabled control that is focusable must
// still report `hovered` and `focused`, or a keyboard user cannot tell where focus is
// (ADR-064 item 9).
bool option_disabled(const ArgsView& args) {
    const ArgValue* value = args.find("disabled", kOptionNames);
    return value != nullptr && value->kind == ArgValue::Kind::Bool && value->boolean;
}

} // namespace

void PressFeedback::attach(const Targets& targets, const Services& services, const ArgsView& args) {
    if (!targets.all_attached() || targets.nodes.empty()) {
        return;
    }
    target_ = targets.nodes.front();
    router_ = services.router;
    disabled_ = option_disabled(args);
    refresh();
}

void PressFeedback::update(const ArgsView& args) {
    // `disabled` is the one reactive option; a change to it changes `pressed` without any scene
    // movement, so the update recomputes rather than assuming the caller noticed.
    disabled_ = option_disabled(args);
    refresh();
}

void PressFeedback::detach() {
    target_ = nullptr;
    router_ = nullptr;
    disabled_ = false;
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
    outputs_[0].boolean = state.pressed && !disabled_;
    outputs_[1].kind = OutputValue::Kind::Bool;
    outputs_[1].boolean = state.hovered;
    outputs_[2].kind = OutputValue::Kind::Bool;
    outputs_[2].boolean = state.focused;
}

} // namespace tessera::ui::behavior
