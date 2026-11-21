// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/focus_trap.hpp"
#include "tessera/ui/behavior/press_feedback.hpp"
#include "tessera/ui/behavior/registry.hpp"
#include "tessera/ui/behavior/roving_focus.hpp"

#include <array>

namespace tessera::ui::behavior {
namespace {

std::unique_ptr<Behavior> make_press_feedback() {
    return std::make_unique<PressFeedback>();
}

std::unique_ptr<Behavior> make_focus_trap() {
    return std::make_unique<FocusTrap>();
}

std::unique_ptr<Behavior> make_roving_focus() {
    return std::make_unique<RovingFocus>();
}

// The registry is a fixed array rather than a map: the set is closed (the library hook manifest is
// the only place that may declare a behavior hook), so a lookup is a linear scan over a handful of
// entries and the build allocates nothing to answer it.
constexpr std::array<Registration, 3> kRegistry{{
    {"press_feedback", &make_press_feedback},
    {"focus_trap", &make_focus_trap},
    {"roving_focus", &make_roving_focus},
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
