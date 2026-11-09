// SPDX-License-Identifier: MIT

#include "tessera/ui/input/key_bindings.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::ui::BindingId;
using tessera::ui::Event;
using tessera::ui::InputDispatchResult;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::KeyBindingRegistry;
using tessera::ui::KeyChord;
using tessera::ui::kInvalidBindingId;
using tessera::ui::Modifier;
using tessera::ui::Node;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "input_accelerator: " << message << "\n";
    }
    return condition;
}

InputEvent key_event(InputEventType type, std::uint32_t key, Modifier modifiers,
                     bool repeat = false) {
    InputEvent event;
    event.type = type;
    event.key = key;
    event.modifiers = modifiers;
    event.repeat = repeat;
    return event;
}

KeyChord chord(std::uint32_t key, Modifier modifiers = Modifier::None) {
    KeyChord value;
    value.key = key;
    value.modifiers = modifiers;
    return value;
}

struct Counter {
    int calls = 0;
};

void count(void* user, const InputEvent&) {
    ++static_cast<Counter*>(user)->calls;
}

struct OrderLog {
    std::vector<std::string> entries;
};

struct RemoveContext {
    KeyBindingRegistry* registry = nullptr;
    BindingId self = kInvalidBindingId;
    BindingId other = kInvalidBindingId;
    int calls = 0;
};

void remove_self(void* user, const InputEvent&) {
    auto* context = static_cast<RemoveContext*>(user);
    ++context->calls;
    context->registry->remove(context->self);
}

void remove_other(void* user, const InputEvent&) {
    auto* context = static_cast<RemoveContext*>(user);
    ++context->calls;
    context->registry->remove(context->other);
}

bool normalization() {
    if (!expect(tessera::ui::normalize_chord_modifiers(Modifier::None) == Modifier::None,
                "no modifiers normalize to none") ||
        !expect(tessera::ui::normalize_chord_modifiers(Modifier::CapsLock | Modifier::NumLock) ==
                    Modifier::None,
                "lock modifiers never form a chord") ||
        !expect(tessera::ui::normalize_chord_modifiers(Modifier::Control | Modifier::Shift) ==
                    tessera::ui::normalize_chord_modifiers(Modifier::Shift | Modifier::Control),
                "modifier order is irrelevant")) {
        return false;
    }
    const KeyChord raw = chord('S', Modifier::Control | Modifier::CapsLock);
    return expect(tessera::ui::normalize_chord(raw).modifiers == Modifier::Control,
                  "normalize_chord drops lock bits") &&
           expect(tessera::ui::normalize_chord(raw).key == 'S', "normalize_chord keeps the key");
}

bool registry_matching() {
    KeyBindingRegistry registry;
    Counter counter;
    const BindingId id = registry.add(chord('S', Modifier::Control), &count, &counter);
    if (!expect(id != kInvalidBindingId, "binding registered") ||
        !expect(registry.size() == 1, "registry size") ||
        !expect(registry.contains(id), "contains the binding")) {
        return false;
    }

    if (!expect(registry.dispatch(key_event(InputEventType::KeyDown, 'S', Modifier::Control)) == id,
                "exact chord fires") ||
        !expect(counter.calls == 1, "callback fired once") ||
        !expect(registry.dispatch(
                    key_event(InputEventType::KeyDown, 'S', Modifier::Control | Modifier::Shift)) ==
                    kInvalidBindingId,
                "modifier superset does not match exactly") ||
        !expect(registry.dispatch(key_event(InputEventType::KeyDown, 'S',
                                            Modifier::Control | Modifier::CapsLock)) == id,
                "lock modifiers are ignored in matching") ||
        !expect(registry.dispatch(key_event(InputEventType::KeyDown, 'S', Modifier::None)) ==
                    kInvalidBindingId,
                "missing modifiers do not match") ||
        !expect(registry.dispatch(key_event(InputEventType::KeyUp, 'S', Modifier::Control)) ==
                    kInvalidBindingId,
                "key up never fires") ||
        !expect(registry.dispatch(key_event(InputEventType::KeyDown, 0, Modifier::Control)) ==
                    kInvalidBindingId,
                "key 0 never fires") ||
        !expect(counter.calls == 2, "only matching key downs fire")) {
        return false;
    }

    return expect(registry.remove(id), "binding removed") &&
           expect(!registry.contains(id), "removed binding is gone") &&
           expect(!registry.remove(id), "double remove is a no-op") &&
           expect(registry.dispatch(key_event(InputEventType::KeyDown, 'S', Modifier::Control)) ==
                      kInvalidBindingId,
                  "removed binding never fires") &&
           expect(registry.add(chord(0), &count, &counter) == kInvalidBindingId,
                  "key 0 registration rejected") &&
           expect(registry.add(chord('A'), nullptr, &counter) == kInvalidBindingId,
                  "null callback rejected");
}

bool superset_and_priority() {
    KeyBindingRegistry registry;
    Counter broad_counter;
    Counter exact_counter;
    const BindingId broad =
        registry.add(chord('K', Modifier::Control), &count, &broad_counter, false, true);
    const BindingId exact = registry.add(chord('K', Modifier::Control), &count, &exact_counter);
    if (!expect(broad != kInvalidBindingId && exact != kInvalidBindingId, "bindings registered")) {
        return false;
    }
    // Exact match wins even though the broad binding was registered first.
    if (!expect(registry.dispatch(key_event(InputEventType::KeyDown, 'K', Modifier::Control)) ==
                    exact,
                "exact binding beats broad") ||
        !expect(exact_counter.calls == 1 && broad_counter.calls == 0, "only the exact fired")) {
        return false;
    }
    if (!expect(registry.dispatch(key_event(InputEventType::KeyDown, 'K',
                                            Modifier::Control | Modifier::Shift)) == broad,
                "broad binding matches extra modifiers")) {
        return false;
    }
    if (!expect(registry.dispatch(key_event(InputEventType::KeyDown, 'K',
                                            Modifier::Control | Modifier::Alt | Modifier::Meta)) ==
                    broad,
                "broad binding matches several extra modifiers")) {
        return false;
    }
    if (!expect(registry.dispatch(key_event(InputEventType::KeyDown, 'K', Modifier::Shift)) ==
                    kInvalidBindingId,
                "broad binding still requires its own modifiers")) {
        return false;
    }

    // Within a class the first registration wins.
    KeyBindingRegistry priority;
    Counter first_counter;
    Counter second_counter;
    priority.add(chord('P', Modifier::Control), &count, &first_counter);
    const BindingId second = priority.add(chord('P', Modifier::Control), &count, &second_counter);
    if (!expect(priority.dispatch(key_event(InputEventType::KeyDown, 'P', Modifier::Control)) !=
                    second,
                "first registration wins") ||
        !expect(first_counter.calls == 1 && second_counter.calls == 0, "first callback ran")) {
        return false;
    }
    priority.remove(1);
    return expect(priority.dispatch(key_event(InputEventType::KeyDown, 'P', Modifier::Control)) ==
                      second,
                  "second binding fires after the first is removed") &&
           expect(second_counter.calls == 1, "second callback ran");
}

bool repeat_semantics() {
    KeyBindingRegistry registry;
    Counter normal;
    Counter repeating;
    registry.add(chord('R', Modifier::Control), &count, &normal);
    registry.add(chord('T', Modifier::Control), &count, &repeating, true);
    if (!expect(registry.dispatch(key_event(InputEventType::KeyDown, 'R', Modifier::Control,
                                            true)) == kInvalidBindingId,
                "normal binding suppresses auto-repeat") ||
        !expect(normal.calls == 0, "normal callback not called on repeat") ||
        !expect(registry.dispatch(key_event(InputEventType::KeyDown, 'R', Modifier::Control,
                                            false)) != kInvalidBindingId,
                "normal binding fires on the first press") ||
        !expect(registry.dispatch(key_event(InputEventType::KeyDown, 'T', Modifier::Control,
                                            false)) != kInvalidBindingId,
                "repeat binding fires on first press") ||
        !expect(registry.dispatch(key_event(InputEventType::KeyDown, 'T', Modifier::Control,
                                            true)) != kInvalidBindingId,
                "repeat binding fires on auto-repeat") ||
        !expect(normal.calls == 1 && repeating.calls == 2, "repeat callbacks counted")) {
        return false;
    }
    return true;
}

bool capacity_and_ids() {
    KeyBindingRegistry registry;
    Counter counter;
    BindingId last = kInvalidBindingId;
    for (std::uint32_t i = 0; i < KeyBindingRegistry::kMaxBindings; ++i) {
        const BindingId id = registry.add(chord('A' + i), &count, &counter);
        if (!expect(id != kInvalidBindingId, "capacity accepts bindings")) {
            return false;
        }
        if (!expect(id > last, "ids increase monotonically")) {
            return false;
        }
        last = id;
    }
    if (!expect(registry.size() == KeyBindingRegistry::kMaxBindings, "registry full") ||
        !expect(registry.add(chord('Z'), &count, &counter) == kInvalidBindingId,
                "overflow returns the invalid id") ||
        !expect(registry.remove(1), "remove for reuse") ||
        !expect(registry.add(chord('Z'), &count, &counter) > last,
                "ids are never reused after removal")) {
        return false;
    }
    registry.clear();
    return expect(registry.size() == 0, "clear empties the registry") &&
           expect(!registry.contains(1), "clear drops ids") &&
           expect(registry.add(chord('Q'), &count, &counter) > last, "ids survive clear");
}

bool mutation_during_dispatch() {
    KeyBindingRegistry registry;
    RemoveContext self;
    self.registry = &registry;
    self.self = registry.add(chord('X', Modifier::Control), &remove_self, &self);
    if (!expect(registry.dispatch(key_event(InputEventType::KeyDown, 'X', Modifier::Control)) ==
                    self.self,
                "self-removing binding fires") ||
        !expect(self.calls == 1, "self-removing callback ran once") ||
        !expect(registry.dispatch(key_event(InputEventType::KeyDown, 'X', Modifier::Control)) ==
                    kInvalidBindingId,
                "self-removed binding does not fire again")) {
        return false;
    }

    KeyBindingRegistry pair;
    RemoveContext first;
    Counter second_counter;
    first.registry = &pair;
    first.self = pair.add(chord('Y', Modifier::Control), &remove_other, &first);
    first.other = pair.add(chord('Y', Modifier::Control), &count, &second_counter);
    if (!expect(pair.dispatch(key_event(InputEventType::KeyDown, 'Y', Modifier::Control)) ==
                    first.self,
                "first binding fires") ||
        !expect(first.calls == 1, "remover ran") ||
        !expect(pair.contains(first.other) == false, "second binding was removed mid-dispatch") ||
        !expect(second_counter.calls == 0, "removed binding did not fire")) {
        return false;
    }

    KeyBindingRegistry cleared;
    Counter counter;
    cleared.add(chord('C', Modifier::Control), &count, &counter);
    if (!expect(cleared.dispatch(key_event(InputEventType::KeyDown, 'C', Modifier::Control)) !=
                    kInvalidBindingId,
                "binding fires before clear") ||
        !expect(cleared.dispatch(key_event(InputEventType::KeyDown, 'C', Modifier::Control)) !=
                    kInvalidBindingId,
                "binding fires again") ||
        !expect(counter.calls == 2, "callback counted")) {
        return false;
    }
    cleared.clear();
    return expect(cleared.dispatch(key_event(InputEventType::KeyDown, 'C', Modifier::Control)) ==
                      kInvalidBindingId,
                  "cleared registry never fires");
}

struct Recorder : Node {
    using Node::Node;

    void on_event(Event& event) override {
        log->entries.push_back(std::to_string(static_cast<int>(event.type)));
        if (consume_handled) {
            event.handled = true;
        }
        if (consume_prevent) {
            event.prevent_default = true;
        }
    }

    OrderLog* log = nullptr;
    bool consume_handled = false;
    bool consume_prevent = false;
};

bool router_accelerators() {
    OrderLog order;
    Recorder root(1);
    root.set_bounds({0, 0, 200, 200});
    root.log = &order;
    auto child_ptr = std::make_unique<Recorder>(2);
    child_ptr->set_bounds({10, 10, 50, 50});
    child_ptr->set_focusable(true);
    child_ptr->log = &order;
    Recorder& child = static_cast<Recorder&>(root.add_child(std::move(child_ptr)));
    auto other_ptr = std::make_unique<Recorder>(3);
    other_ptr->set_bounds({100, 10, 50, 50});
    other_ptr->set_focusable(true);
    other_ptr->log = &order;
    root.add_child(std::move(other_ptr));

    InputRouter router;
    Counter counter;
    const BindingId save =
        router.accelerators().add(chord('S', Modifier::Control), &count, &counter);
    if (!expect(save != kInvalidBindingId, "router binding registered") ||
        !expect(router.focus_manager().request_focus(&child), "focus child")) {
        return false;
    }

    order.entries.clear();
    child.log = &order;
    const InputDispatchResult fired =
        router.dispatch(root, key_event(InputEventType::KeyDown, 'S', Modifier::Control));
    if (!expect(fired.dispatched, "key dispatched") ||
        !expect(fired.accelerator_fired == save, "accelerator fired id reported") ||
        !expect(counter.calls == 1, "accelerator callback ran") ||
        !expect(order.entries.size() == 3 && order.entries[0] == order.entries[1] &&
                    order.entries[1] == order.entries[2],
                "route log still contains the focused-path key events")) {
        return false;
    }

    // A focused handler can consume the key; the accelerator must not fire.
    order.entries.clear();
    child.consume_prevent = true;
    const InputDispatchResult consumed =
        router.dispatch(root, key_event(InputEventType::KeyDown, 'S', Modifier::Control));
    child.consume_prevent = false;
    if (!expect(consumed.accelerator_fired == kInvalidBindingId,
                "consumed key does not fire the accelerator") ||
        !expect(!consumed.default_prevented,
                "prevent_default is only reported for real default actions") ||
        !expect(counter.calls == 1, "consumed key leaves the callback count")) {
        return false;
    }

    // handled is an equivalent consume signal.
    order.entries.clear();
    child.consume_handled = true;
    const InputDispatchResult handled =
        router.dispatch(root, key_event(InputEventType::KeyDown, 'S', Modifier::Control));
    child.consume_handled = false;
    if (!expect(handled.accelerator_fired == kInvalidBindingId,
                "handled key does not fire the accelerator")) {
        return false;
    }

    // Key up never fires.
    order.entries.clear();
    const InputDispatchResult up =
        router.dispatch(root, key_event(InputEventType::KeyUp, 'S', Modifier::Control));
    if (!expect(up.accelerator_fired == kInvalidBindingId, "key up does not fire")) {
        return false;
    }

    // Auto-repeat is opt-in per binding.
    child.consume_prevent = false;
    order.entries.clear();
    const InputDispatchResult repeat =
        router.dispatch(root, key_event(InputEventType::KeyDown, 'S', Modifier::Control, true));
    if (!expect(repeat.accelerator_fired == kInvalidBindingId, "repeat suppressed by default")) {
        return false;
    }

    // A Tab accelerator suppresses the router's Tab focus default action.
    const BindingId tab =
        router.accelerators().add(chord(tessera::ui::key::kTab), &count, &counter);
    router.focus_manager().request_focus(&child);
    order.entries.clear();
    const InputDispatchResult tabbed = router.dispatch(
        root, key_event(InputEventType::KeyDown, tessera::ui::key::kTab, Modifier::None));
    if (!expect(tab != kInvalidBindingId, "tab binding registered") ||
        !expect(tabbed.accelerator_fired == tab, "tab accelerator fired") ||
        !expect(router.focus_manager().focused() == &child, "tab traversal suppressed") ||
        !expect(!tabbed.focus_moved, "no focus movement when the accelerator fired")) {
        return false;
    }

    // Without focus app-level accelerators still fire (menus keep working).
    router.focus_manager().clear_focus();
    order.entries.clear();
    const InputDispatchResult unfocused =
        router.dispatch(root, key_event(InputEventType::KeyDown, 'S', Modifier::Control));
    return expect(unfocused.accelerator_fired == save, "accelerator fires without focus") &&
           expect(order.entries.empty(), "no focused node route log") &&
           expect(counter.calls == 3, "callback count across the script");
}

bool determinism_and_passthrough() {
    const auto run = [](bool with_bindings, std::vector<std::string>& route) {
        Recorder root(1);
        root.set_bounds({0, 0, 200, 200});
        auto child_ptr = std::make_unique<Recorder>(2);
        child_ptr->set_bounds({10, 10, 50, 50});
        child_ptr->set_focusable(true);
        Recorder& child = static_cast<Recorder&>(root.add_child(std::move(child_ptr)));
        OrderLog log;
        log.entries = {};
        root.log = &log;
        child.log = &log;
        InputRouter router;
        if (with_bindings) {
            Counter counter;
            router.accelerators().add(chord('J', Modifier::Control), &count, &counter);
        }
        router.focus_manager().request_focus(&child);
        const InputEventType down = InputEventType::KeyDown;
        const InputEventType up = InputEventType::KeyUp;
        router.dispatch(root, key_event(down, 'A', Modifier::None));
        router.dispatch(root, key_event(up, 'A', Modifier::None));
        router.dispatch(root, key_event(down, 'B', Modifier::Shift));
        router.dispatch(root, key_event(down, 'J', Modifier::Alt));
        route = log.entries;
    };

    std::vector<std::string> plain_route;
    std::vector<std::string> bound_route;
    run(false, plain_route);
    run(true, bound_route);
    return expect(plain_route == bound_route,
                  "no matching binding keeps the route log byte-identical") &&
           expect(!plain_route.empty(), "route log recorded");
}

} // namespace

int main() {
    if (!normalization()) {
        return 1;
    }
    if (!registry_matching()) {
        return 2;
    }
    if (!superset_and_priority()) {
        return 3;
    }
    if (!repeat_semantics()) {
        return 4;
    }
    if (!capacity_and_ids()) {
        return 5;
    }
    if (!mutation_during_dispatch()) {
        return 6;
    }
    if (!router_accelerators()) {
        return 7;
    }
    if (!determinism_and_passthrough()) {
        return 8;
    }

    std::cout << "input_accelerator: ok\n";
    return 0;
}
