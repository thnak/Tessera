// SPDX-License-Identifier: MIT

// #656: the `transition` and `deferred_value` engine behaviors behind `useTransition` and
// `useDeferredValue`, driven directly through the registry. (The generated base reads the
// component's own pending model instead, because `Instance::attach` takes no target-less behavior;
// the codegen_pending_tsx test proves that path.)

#include "tessera/ui/behavior/registry.hpp"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace behavior = tessera::ui::behavior;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void test_transition() {
    auto transition = behavior::create_behavior("transition");
    check(transition != nullptr, "transition is registered");
    if (!transition) {
        return;
    }
    const behavior::Services services;
    const behavior::Targets none{};
    std::array<behavior::ArgValue, 1> args{behavior::make_arg(false)};
    transition->attach(none, services, behavior::ArgsView{args});
    check(transition->outputs().size() == 1 && !transition->outputs()[0].boolean,
          "isPending starts false");
    args[0] = behavior::make_arg(true);
    transition->update(behavior::ArgsView{args});
    check(transition->outputs()[0].boolean, "isPending follows the pending option");
    transition->detach();
    check(!transition->outputs()[0].boolean, "detach returns the default");
}

void test_deferred_value() {
    auto deferred = behavior::create_behavior("deferred_value");
    check(deferred != nullptr, "deferred_value is registered");
    if (!deferred) {
        return;
    }
    const behavior::Services services;
    const behavior::Targets none{};
    std::array<behavior::ArgValue, 2> args{behavior::make_arg("fast"), behavior::make_arg(false)};
    deferred->attach(none, services, behavior::ArgsView{args});
    check(deferred->outputs()[0].kind == behavior::OutputValue::Kind::String &&
              deferred->outputs()[0].text == "fast",
          "the first value settles");
    args[0] = behavior::make_arg("slow");
    args[1] = behavior::make_arg(true);
    deferred->update(behavior::ArgsView{args});
    check(deferred->outputs()[0].text == "fast", "held: the last settled value");
    args[1] = behavior::make_arg(false);
    deferred->update(behavior::ArgsView{args});
    check(deferred->outputs()[0].text == "slow", "released: the new value settles");

    // A number keeps its kind.
    std::array<behavior::ArgValue, 2> numbers{behavior::make_arg(1.0), behavior::make_arg(false)};
    auto count = behavior::create_behavior("deferred_value");
    count->attach(none, services, behavior::ArgsView{numbers});
    check(count->outputs()[0].kind == behavior::OutputValue::Kind::Number &&
              count->outputs()[0].number == 1.0,
          "a numeric value keeps its kind");
}

} // namespace

int main() {
    test_transition();
    test_deferred_value();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pending_behavior: ok\n";
    return 0;
}
