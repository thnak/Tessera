// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/behavior/behavior.hpp"

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

// The behavior registry (docs/design/behavior-hooks.md section 5): name -> factory. The library
// hook manifest names a behavior, and a manifest entry that names a behavior the registry lacks
// fails a unit test, so the two cannot drift.
namespace tessera::ui::behavior {

// One registered behavior: the name the manifest uses and the factory that builds an instance.
struct Registration {
    std::string_view name;
    std::unique_ptr<Behavior> (*create)();
};

// Every behavior this build knows, in a stable order (the order the manifest test walks). A build
// with the experimental behavior path off still registers the names, so the manifest test is the
// same in both configurations; only the generated code that would use them is gated.
[[nodiscard]] std::span<const Registration> registry();

// The factory for `name`, or nullptr when the registry has no such behavior.
[[nodiscard]] std::unique_ptr<Behavior> create_behavior(std::string_view name);

// True when `name` is a registered behavior. The manifest test and the code generator both use this
// rather than walking the registry themselves.
[[nodiscard]] bool is_registered(std::string_view name);

} // namespace tessera::ui::behavior
