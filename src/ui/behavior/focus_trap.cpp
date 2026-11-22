// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/focus_trap.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"

namespace tessera::ui::behavior {
namespace {

// The manifest's option order: `active`, `restoreFocus`. A behavior reads the field it expects and
// ignores the rest, so a missing option falls back to the manifest default.
constexpr std::string_view kOptionNames[] = {"active", "restoreFocus"};

bool option_bool(const ArgsView& args, std::string_view name, bool fallback) {
    const ArgValue* value = args.find(name, kOptionNames);
    if (value == nullptr || value->kind != ArgValue::Kind::Bool) {
        return fallback;
    }
    return value->boolean;
}

} // namespace

void FocusTrap::attach(const Targets& targets, const Services& services, const ArgsView& args) {
    if (!targets.all_attached() || targets.nodes.empty()) {
        return;
    }
    target_ = targets.nodes.front();
    router_ = services.router;
    apply(option_bool(args, "active", true));
}

void FocusTrap::update(const ArgsView& args) {
    if (target_ == nullptr) {
        return;
    }
    apply(option_bool(args, "active", true));
}

void FocusTrap::detach() {
    // A trap that outlives its component would confine focus to a subtree that is about to be
    // destroyed, so detach always clears the flag it set. `restoreFocus` is the host's job: it
    // reconciles the tree through `sync_focus_trap`, which restores the focus held before the trap.
    if (target_ != nullptr && applied_) {
        target_->set_focus_trap(false);
    }
    target_ = nullptr;
    router_ = nullptr;
    applied_ = false;
    outputs_ = {};
}

std::span<const OutputValue> FocusTrap::outputs() const {
    return outputs_;
}

void FocusTrap::apply(bool active) {
    if (target_ == nullptr) {
        return;
    }
    target_->set_focus_trap(active);
    applied_ = active;
    // The output reports whether the trap is on, not whether it currently holds focus: a trap whose
    // subtree has no focusable node is still active, and a component that reads `trap.active` is
    // asking about its own option, not about where focus happens to be.
    outputs_[0].kind = OutputValue::Kind::Bool;
    outputs_[0].boolean = active;
}

} // namespace tessera::ui::behavior
