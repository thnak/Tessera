// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/press_feedback.hpp"
#include "tessera/ui/behavior/registry.hpp"

#include <array>

namespace tessera::ui::behavior {
namespace {

std::unique_ptr<Behavior> make_press_feedback() {
    return std::make_unique<PressFeedback>();
}

// The registry is a fixed array rather than a map: the set is closed (the library hook manifest is
// the only place that may declare a behavior hook), so a lookup is a linear scan over a handful of
// entries and the build allocates nothing to answer it.
constexpr std::array<Registration, 1> kRegistry{{
    {"press_feedback", &make_press_feedback},
}};

} // namespace

std::span<const Registration> registry() {
    return kRegistry;
}

std::unique_ptr<Behavior> create_behavior(std::string_view name) {
    for (const Registration& entry : kRegistry) {
        if (entry.name == name) {
            return entry.create();
        }
    }
    return nullptr;
}

bool is_registered(std::string_view name) {
    for (const Registration& entry : kRegistry) {
        if (entry.name == name) {
            return true;
        }
    }
    return false;
}

} // namespace tessera::ui::behavior
