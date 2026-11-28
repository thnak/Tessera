// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/transition.hpp"

namespace tessera::ui::behavior {
namespace {

// The option order: `pending`.
constexpr std::string_view kOptionNames[] = {"pending"};

} // namespace

void Transition::attach(const Targets& /*targets*/, const Services& /*services*/,
                        const ArgsView& args) {
    apply(args);
}

void Transition::update(const ArgsView& args) {
    apply(args);
}

void Transition::detach() {
    outputs_ = {};
}

std::span<const OutputValue> Transition::outputs() const {
    return outputs_;
}

void Transition::apply(const ArgsView& args) {
    const ArgValue* pending = args.find("pending", kOptionNames);
    outputs_[0].kind = OutputValue::Kind::Bool;
    outputs_[0].boolean =
        pending != nullptr && pending->kind == ArgValue::Kind::Bool && pending->boolean;
}

} // namespace tessera::ui::behavior
