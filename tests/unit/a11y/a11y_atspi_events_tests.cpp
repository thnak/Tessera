// SPDX-License-Identifier: MIT
//
// Accessibility event queue: bounded capacity, coalescing by (kind, node, detail), FIFO delivery,
// and the event factories the scene code posts.

#include "a11y_dbus_test_support.hpp"
#include "tessera/ui/accessibility/atspi_events.hpp"

#include <array>
#include <string>
#include <vector>

namespace {

using namespace a11y_dbus_test;
using tessera::ui::a11y::A11yEvent;
using tessera::ui::a11y::event_detail;
using tessera::ui::a11y::event_signal_member;
using tessera::ui::a11y::event_value;
using tessera::ui::a11y::EventKind;
using tessera::ui::a11y::EventQueue;
using tessera::ui::a11y::kMaxEventTextBytes;
using tessera::ui::a11y::kMaxQueuedEvents;
using tessera::ui::a11y::make_bounds_event;
using tessera::ui::a11y::make_focus_event;
using tessera::ui::a11y::make_property_event;
using tessera::ui::a11y::make_state_event;
using tessera::ui::a11y::make_visible_data_event;
using tessera::ui::a11y::set_event_text;

bool factories() {
    const A11yEvent focus = make_focus_event(3);
    const A11yEvent state = make_state_event(4, "checked", false);
    const A11yEvent property = make_property_event(5, "accessible-name", "Save");
    const A11yEvent bounds = make_bounds_event(6);
    const A11yEvent visible = make_visible_data_event(7);
    return expect(focus.kind == EventKind::StateChanged && focus.node == 3, "focus event kind") &&
           expect(focus.detail1 == 1, "focus sets the state") &&
           expect_equal(event_detail(focus), "focused", "focus detail name") &&
           expect(state.detail1 == 0, "cleared state is 0") &&
           expect_equal(event_detail(state), "checked", "state detail name") &&
           expect(property.kind == EventKind::PropertyChanged, "property event kind") &&
           expect_equal(event_detail(property), "accessible-name", "property name") &&
           expect_equal(event_value(property), "Save", "property value") &&
           expect(bounds.kind == EventKind::BoundsChanged, "bounds event kind") &&
           expect(visible.kind == EventKind::VisibleDataChanged, "visible data event kind") &&
           expect_equal(event_signal_member(EventKind::StateChanged), "StateChanged",
                        "state signal member") &&
           expect_equal(event_signal_member(EventKind::PropertyChanged), "PropertyChange",
                        "property signal member");
}

bool bounded_text() {
    A11yEvent event;
    set_event_text(event.detail, "short");
    if (!expect_equal(event_detail(event), "short", "short detail is copied")) {
        return false;
    }
    // A multi-byte sequence straddling the cap must be dropped whole, never split.
    std::string long_text(kMaxEventTextBytes - 2, 'a');
    long_text.append("\xC3\xA9"); // "é" in UTF-8, the last code point does not fit
    long_text.append("tail");
    set_event_text(event.detail, long_text);
    const std::string_view stored = event_detail(event);
    return expect(stored.size() <= kMaxEventTextBytes - 1, "detail is bounded") &&
           expect(stored.back() == 'a', "truncation stops at a code-point boundary");
}

bool fifo_order() {
    EventQueue queue;
    for (std::uint16_t node = 0; node < 4; ++node) {
        queue.push(make_state_event(node, "focused", true));
    }
    std::array<A11yEvent, 8> batch{};
    const std::size_t taken = queue.take(batch);
    if (!expect(taken == 4, "four events")) {
        return false;
    }
    for (std::uint16_t node = 0; node < 4; ++node) {
        if (!expect(batch[node].node == node, "FIFO order")) {
            return false;
        }
    }
    return expect(queue.size() == 0, "queue drains");
}

bool coalescing() {
    EventQueue queue;
    queue.push(make_property_event(2, "accessible-name", "Fir"));
    queue.push(make_property_event(2, "accessible-name", "First"));
    queue.push(make_property_event(2, "accessible-name", "First!"));
    if (!expect(queue.size() == 1, "same key coalesces to one slot") ||
        !expect(queue.coalesced() == 2, "coalescing is counted")) {
        return false;
    }
    std::array<A11yEvent, 4> batch{};
    const std::size_t taken = queue.take(batch);
    return expect(taken == 1, "one coalesced event") &&
           expect_equal(event_value(batch[0]), "First!", "the last value wins");
}

bool distinct_keys_do_not_coalesce() {
    EventQueue queue;
    queue.push(make_state_event(1, "focused", true));
    queue.push(make_state_event(1, "focused", false)); // same key: coalesces
    queue.push(make_state_event(1, "checked", true));  // different detail: new slot
    queue.push(make_state_event(2, "focused", true));  // different node: new slot
    std::array<A11yEvent, 8> batch{};
    const std::size_t taken = queue.take(batch);
    return expect(taken == 3, "three distinct keys") &&
           expect(batch[0].detail1 == 0, "coalesced state keeps the last value") &&
           expect_equal(event_detail(batch[1]), "checked", "second key") &&
           expect(batch[2].node == 2, "third key");
}

bool bounded_capacity() {
    EventQueue queue;
    for (std::size_t index = 0; index < kMaxQueuedEvents; ++index) {
        if (!expect(
                queue.push(make_state_event(static_cast<std::uint16_t>(index), "focused", true)),
                "event fits")) {
            return false;
        }
    }
    if (!expect(queue.size() == kMaxQueuedEvents, "queue is full") ||
        !expect(!queue.push(make_state_event(999, "focused", true)), "overflow is refused") ||
        !expect(queue.dropped() == 1, "drop is counted")) {
        return false;
    }
    std::array<A11yEvent, kMaxQueuedEvents + 4> batch{};
    const std::size_t taken = queue.take(batch);
    queue.clear();
    return expect(taken == kMaxQueuedEvents, "all queued events delivered") &&
           expect(queue.size() == 0, "clear empties the queue");
}

bool partial_take() {
    EventQueue queue;
    queue.push(make_bounds_event(1));
    queue.push(make_bounds_event(2));
    queue.push(make_bounds_event(3));
    std::array<A11yEvent, 2> batch{};
    if (!expect(queue.take(batch) == 2, "takes what fits")) {
        return false;
    }
    std::array<A11yEvent, 4> rest{};
    return expect(queue.take(rest) == 1, "remaining event") &&
           expect(rest[0].node == 3, "remaining order");
}

bool empty_take() {
    EventQueue queue;
    std::array<A11yEvent, 2> batch{};
    return expect(queue.take(batch) == 0, "empty queue takes nothing") &&
           expect(queue.take({}) == 0, "empty destination takes nothing");
}

} // namespace

int main() {
    if (!factories()) {
        return 1;
    }
    if (!bounded_text()) {
        return 2;
    }
    if (!fifo_order()) {
        return 3;
    }
    if (!coalescing()) {
        return 4;
    }
    if (!distinct_keys_do_not_coalesce()) {
        return 5;
    }
    if (!bounded_capacity()) {
        return 6;
    }
    if (!partial_take()) {
        return 7;
    }
    if (!empty_take()) {
        return 8;
    }
    std::cout << "a11y_atspi_events: ok\n";
    return 0;
}
