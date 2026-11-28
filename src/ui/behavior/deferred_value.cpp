// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/deferred_value.hpp"

namespace tessera::ui::behavior {
namespace {

// The option order: `value`, `hold`.
constexpr std::string_view kOptionNames[] = {"value", "hold"};

OutputValue output_of(const ArgValue& value) {
    OutputValue out;
    switch (value.kind) {
    case ArgValue::Kind::Number:
        out.kind = OutputValue::Kind::Number;
        out.number = value.number;
        break;
    case ArgValue::Kind::String:
        out.kind = OutputValue::Kind::String;
        out.text = value.text;
        break;
    default:
        out.kind = OutputValue::Kind::Bool;
        out.boolean = value.boolean;
        break;
    }
    return out;
}

} // namespace

void DeferredValue::attach(const Targets& /*targets*/, const Services& /*services*/,
                           const ArgsView& args) {
    // The first value is the settled value: nothing was pending before the component existed.
    if (const ArgValue* value = args.find("value", kOptionNames); value != nullptr) {
        outputs_[0] = output_of(*value);
    }
}

void DeferredValue::update(const ArgsView& args) {
    apply(args);
}

void DeferredValue::detach() {
    outputs_ = {};
}

std::span<const OutputValue> DeferredValue::outputs() const {
    return outputs_;
}

void DeferredValue::apply(const ArgsView& args) {
    const ArgValue* hold = args.find("hold", kOptionNames);
    if (hold != nullptr && hold->kind == ArgValue::Kind::Bool && hold->boolean) {
        return; // a transition has a pending read: keep the last settled value
    }
    if (const ArgValue* value = args.find("value", kOptionNames); value != nullptr) {
        outputs_[0] = output_of(*value);
    }
}

} // namespace tessera::ui::behavior
