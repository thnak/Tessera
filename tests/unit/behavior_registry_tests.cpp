// SPDX-License-Identifier: MIT
//
// The behavior registry (ADR-064 item 7, spike #658): the name -> factory table the library hook
// manifest is checked against, and the contract that a manifest entry naming a behavior the
// registry lacks fails rather than silently doing nothing.

#include "tessera/ui/behavior/press_feedback.hpp"
#include "tessera/ui/behavior/registry.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::behavior::create_behavior;
using tessera::ui::behavior::is_registered;
using tessera::ui::behavior::PressFeedback;
using tessera::ui::behavior::registry;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "behavior_registry: " << message << "\n";
    }
    return condition;
}

// The registry is the closed set the manifest may name. A behavior that is not here cannot be
// declared by any hook, which is what keeps the manifest and the engine from drifting.
bool test_registry_is_the_closed_set() {
    bool ok = expect(!registry().empty(), "the registry is not empty");
    ok = expect(is_registered("press_feedback"), "press_feedback is registered") && ok;
    ok = expect(is_registered("focus_trap"), "focus_trap is registered") && ok;
    ok = expect(is_registered("roving_focus"), "roving_focus is registered") && ok;
    ok = expect(is_registered("scroll_lock"), "scroll_lock is registered") && ok;
    ok = expect(is_registered("hotkeys"), "hotkeys is registered") && ok;
    ok = expect(!is_registered("typeahead"),
                "a behavior with no implementation is not registered") &&
         ok;
    ok = expect(!is_registered(""), "the empty name is not registered") && ok;
    return ok;
}

// Every entry builds a distinct instance, so two components using the same behavior never share
// state.
bool test_factory_builds_a_fresh_instance() {
    const auto first = create_behavior("press_feedback");
    const auto second = create_behavior("press_feedback");
    bool ok = expect(first != nullptr, "the factory builds press_feedback");
    ok = expect(second != nullptr, "the factory builds a second press_feedback") && ok;
    ok = expect(first.get() != second.get(), "two instances are distinct") && ok;
    ok = expect(dynamic_cast<PressFeedback*>(first.get()) != nullptr,
                "the factory builds the type the name promises") &&
         ok;
    return ok;
}

// An unknown name is a null factory rather than a default-constructed behavior: the caller reports
// `TESSERA_BEHAVIOR_UNKNOWN` instead of attaching something that does nothing.
bool test_unknown_name_has_no_factory() {
    bool ok = expect(create_behavior("nope") == nullptr, "an unknown name has no factory");
    ok = expect(create_behavior("") == nullptr, "the empty name has no factory") && ok;
    return ok;
}

// The registry order is stable, so the manifest test and the documentation walk it the same way.
bool test_registry_order_is_stable() {
    const auto first = registry();
    const auto second = registry();
    bool ok = expect(first.size() == second.size(), "the registry size is stable");
    for (std::size_t index = 0; index < first.size() && index < second.size(); ++index) {
        ok = expect(first[index].name == second[index].name, "the registry order is stable") && ok;
    }
    return ok;
}

} // namespace

int main() {
    bool ok = test_registry_is_the_closed_set();
    ok = test_factory_builds_a_fresh_instance() && ok;
    ok = test_unknown_name_has_no_factory() && ok;
    ok = test_registry_order_is_stable() && ok;
    if (!ok) {
        std::cerr << "behavior_registry: FAILED\n";
        return 1;
    }
    std::cout << "behavior_registry: ok\n";
    return 0;
}
