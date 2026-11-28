// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/announce.hpp"
#include "tessera/ui/behavior/clipboard_copy.hpp"
#include "tessera/ui/behavior/deferred_value.hpp"
#include "tessera/ui/behavior/focus_trap.hpp"
#include "tessera/ui/behavior/hotkeys.hpp"
#include "tessera/ui/behavior/interactive_role.hpp"
#include "tessera/ui/behavior/overflow.hpp"
#include "tessera/ui/behavior/press_feedback.hpp"
#include "tessera/ui/behavior/registry.hpp"
#include "tessera/ui/behavior/roving_focus.hpp"
#include "tessera/ui/behavior/scroll_lock.hpp"
#include "tessera/ui/behavior/transition.hpp"

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

std::unique_ptr<Behavior> make_scroll_lock() {
    return std::make_unique<ScrollLock>();
}

std::unique_ptr<Behavior> make_hotkeys() {
    return std::make_unique<Hotkeys>();
}

std::unique_ptr<Behavior> make_clipboard_copy() {
    return std::make_unique<ClipboardCopy>();
}

std::unique_ptr<Behavior> make_overflow() {
    return std::make_unique<Overflow>();
}

std::unique_ptr<Behavior> make_interactive_role() {
    return std::make_unique<InteractiveRole>();
}

std::unique_ptr<Behavior> make_announce() {
    return std::make_unique<Announce>();
}

std::unique_ptr<Behavior> make_transition() {
    return std::make_unique<Transition>();
}

std::unique_ptr<Behavior> make_deferred_value() {
    return std::make_unique<DeferredValue>();
}

// The registry is a fixed array rather than a map: the set is closed (the library hook manifest is
// the only place that may declare a behavior hook), so a lookup is a linear scan over a handful of
// entries and the build allocates nothing to answer it.
constexpr std::array<Registration, 11> kRegistry{{
    {"press_feedback", &make_press_feedback},
    {"focus_trap", &make_focus_trap},
    {"roving_focus", &make_roving_focus},
    {"scroll_lock", &make_scroll_lock},
    {"hotkeys", &make_hotkeys},
    {"clipboard_copy", &make_clipboard_copy},
    {"overflow", &make_overflow},
    {"interactive_role", &make_interactive_role},
    {"announce", &make_announce},
    {"transition", &make_transition},
    {"deferred_value", &make_deferred_value},
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
