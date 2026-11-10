// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

namespace tessera::ui::a11y {

// Bounded, coalescing accessibility event queue (docs/specs/accessibility-v1.md section 10,
// docs/design/accessibility.md section 8). Scene/UI code posts semantic events; the AT-SPI2 bridge
// thread drains them and writes D-Bus signals. The queue is the only thing crossing that boundary,
// so it is fixed-capacity, lock-protected for a few microseconds, and never grows.
//
// Coalescing replaces a queued event with the same (kind, node, detail) key in place: a value that
// changed five times before the bridge drained it is announced once with the final value, which is
// exactly the "no flooding" rule for progress and text changes.
enum class EventKind : std::uint8_t {
    StateChanged = 0,    // AT-SPI2 Event.Object.StateChanged
    PropertyChanged = 1, // AT-SPI2 Event.Object.PropertyChange
    BoundsChanged = 2,   // AT-SPI2 Event.Object.BoundsChanged
    VisibleDataChanged = 3,
};

inline constexpr std::size_t kMaxEventTextBytes = 64;
inline constexpr std::size_t kMaxQueuedEvents = 64;

struct A11yEvent {
    EventKind kind = EventKind::StateChanged;
    A11yIndex node = kInvalidIndex;
    std::int32_t detail1 = 0; // StateChanged: 1 when the state is set, 0 when cleared
    std::int32_t detail2 = 0;
    // State or property name ("focused", "accessible-name"), and the new property value.
    std::array<char, kMaxEventTextBytes> detail{};
    std::array<char, kMaxEventTextBytes> value{};
};

// Copies at most kMaxEventTextBytes - 1 bytes at a UTF-8 boundary and NUL-terminates.
void set_event_text(std::array<char, kMaxEventTextBytes>& target, std::string_view text);
[[nodiscard]] std::string_view event_detail(const A11yEvent& event);
[[nodiscard]] std::string_view event_value(const A11yEvent& event);

[[nodiscard]] A11yEvent make_state_event(A11yIndex node, std::string_view state, bool enabled);
[[nodiscard]] A11yEvent make_focus_event(A11yIndex node);
[[nodiscard]] A11yEvent make_property_event(A11yIndex node, std::string_view property,
                                            std::string_view value);
[[nodiscard]] A11yEvent make_bounds_event(A11yIndex node);
[[nodiscard]] A11yEvent make_visible_data_event(A11yIndex node);

// D-Bus signal member for `EventKind` on the `org.a11y.atspi.Event.Object` interface.
[[nodiscard]] std::string_view event_signal_member(EventKind kind);

class EventQueue {
public:
    // Producer side (UI thread). Returns false when the event was dropped because the queue is
    // full; `dropped()` counts it. Identical keys coalesce instead of consuming a slot.
    bool push(const A11yEvent& event);
    // Consumer side (bridge thread): moves up to `out.size()` events, oldest first.
    std::size_t take(std::span<A11yEvent> out);
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t dropped() const;
    [[nodiscard]] std::size_t coalesced() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::array<A11yEvent, kMaxQueuedEvents> events_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::size_t dropped_ = 0;
    std::size_t coalesced_ = 0;
};

} // namespace tessera::ui::a11y
