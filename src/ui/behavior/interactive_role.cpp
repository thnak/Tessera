// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/interactive_role.hpp"
#include "tessera/ui/scene/node.hpp"

namespace tessera::ui::behavior {
namespace {

// The manifest's option order: `interactive`.
constexpr std::string_view kOptionNames[] = {"interactive"};

bool option_bool(const ArgsView& args, std::string_view name, bool fallback) {
    const ArgValue* value = args.find(name, kOptionNames);
    if (value == nullptr || value->kind != ArgValue::Kind::Bool) {
        return fallback;
    }
    return value->boolean;
}

} // namespace

void InteractiveRole::attach(const Targets& targets, const Services& services,
                             const ArgsView& args) {
    (void)services; // the flag is on the node, not in the interaction state
    if (!targets.all_attached() || targets.nodes.empty()) {
        return;
    }
    target_ = targets.nodes.front();
    apply(option_bool(args, "interactive", true));
}

void InteractiveRole::update(const ArgsView& args) {
    if (target_ == nullptr) {
        return;
    }
    apply(option_bool(args, "interactive", true));
}

void InteractiveRole::detach() {
    // A flag that outlives its component would tell a descendant it is nested inside a control that
    // no longer exists, so detach always clears what it set.
    if (target_ != nullptr && applied_) {
        target_->set_interactive_parent(false);
    }
    target_ = nullptr;
    applied_ = false;
    outputs_ = {};
}

std::span<const OutputValue> InteractiveRole::outputs() const {
    return outputs_;
}

bool InteractiveRole::has_interactive_ancestor(const Node& node) {
    for (const Node* current = node.parent(); current != nullptr; current = current->parent()) {
        if (current->interactive_parent()) {
            return true;
        }
    }
    return false;
}

void InteractiveRole::apply(bool interactive) {
    if (target_ == nullptr) {
        return;
    }
    target_->set_interactive_parent(interactive);
    applied_ = interactive;
    // The output reports whether THIS node is nested inside another interactive surface, which is
    // what a component needs to decide whether to render its own control. A node that is itself the
    // interactive surface is not nested in itself.
    outputs_[0].kind = OutputValue::Kind::Bool;
    outputs_[0].boolean = interactive && has_interactive_ancestor(*target_);
}

} // namespace tessera::ui::behavior
