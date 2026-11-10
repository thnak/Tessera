// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"
#include "tessera/ui/accessibility/atspi_events.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

namespace tessera::ui::a11y {

// Events the Windows bridge raises (docs/specs/accessibility-v1.md section 12.2). The shapes mirror
// the AT-SPI2 `A11yEvent`/`EventQueue` model in atspi_events.hpp: bounded text, a coalescing queue,
// and counters. Only notification state crosses the bridge boundary; clients always re-query the
// provider for values, so a dropped event loses a hint, never correctness.
enum class UiaEventKind : std::uint8_t {
    StructureChanged = 0, // UiaRaiseStructureChangedEvent
    PropertyChanged = 1,  // UiaRaiseAutomationPropertyChangedEvent
    FocusChanged = 2,     // UiaRaiseAutomationEvent(UIA_AutomationFocusChangedEventId)
};

// UIA `StructureChangeType` values the diff raises; `uia_sdk_probe.cpp` static_asserts the spelling
// against the SDK on Windows. The bulk variants are listed so the gap to `ChildrenReordered` (5) is
// explicit.
inline constexpr std::int32_t kUiaChildAdded = 0;
inline constexpr std::int32_t kUiaChildRemoved = 1;
inline constexpr std::int32_t kUiaChildrenInvalidated = 2;
inline constexpr std::int32_t kUiaChildrenBulkAdded = 3;
inline constexpr std::int32_t kUiaChildrenBulkRemoved = 4;
inline constexpr std::int32_t kUiaChildrenReordered = 5;

inline constexpr std::size_t kMaxQueuedUiaEvents = 64;

struct UiaEvent {
    UiaEventKind kind = UiaEventKind::PropertyChanged;
    A11yIndex node = kInvalidIndex;
    // PropertyChanged: UIA property id. StructureChanged: the `StructureChangeType`. FocusChanged:
    // 0.
    std::int32_t property = 0;
    // New string value for text-valued properties; kept so coalescing can tell a real change from
    // a duplicate and so diagnostics can show what changed.
    std::array<char, kMaxEventTextBytes> value{};
};

[[nodiscard]] UiaEvent make_uia_structure_event(A11yIndex node,
                                                std::int32_t change_type = kUiaChildrenInvalidated);
[[nodiscard]] UiaEvent make_uia_property_event(A11yIndex node, std::int32_t property,
                                               std::string_view value = {});
[[nodiscard]] UiaEvent make_uia_focus_event(A11yIndex node);
[[nodiscard]] std::string_view uia_event_value(const UiaEvent& event);

// Bounded, coalescing event queue (UI thread produces, bridge thread raises).
//
// Coalescing and dedup rules:
// - PropertyChanged coalesces per (node, property): the newest value replaces the queued one. The
//   same value again is a dedup (counted, no slot touched), so a value that changed five times
//   before a flush is raised once with the final value.
// - StructureChanged coalesces per node: at most one pending event per node, newest change type
//   wins. A client invalidates the subtree once, not per mutation.
// - FocusChanged keeps at most one pending event: a newer focus replaces the queued one.
//
// Drop policy: the queue holds kMaxQueuedUiaEvents events. When a new event needs a slot, the
// oldest is dropped (newest notification wins), counted in `dropped()`, and `push` returns false so
// a host can count the overflow. The queue never grows and never blocks beyond one mutex.
class UiaEventQueue {
public:
    // UI thread. Returns false when an older queued event had to be dropped to make room.
    bool push(const UiaEvent& event);
    // Bridge thread: moves up to `out.size()` events, oldest first.
    std::size_t take(std::span<UiaEvent> out);
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t dropped() const;
    [[nodiscard]] std::size_t coalesced() const;
    [[nodiscard]] std::size_t deduped() const;
    void clear();

private:
    [[nodiscard]] std::size_t find_locked(const UiaEvent& event) const;
    void append_locked(const UiaEvent& event);

    mutable std::mutex mutex_;
    std::array<UiaEvent, kMaxQueuedUiaEvents> events_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
    std::size_t dropped_ = 0;
    std::size_t coalesced_ = 0;
    std::size_t deduped_ = 0;
};

struct UiaEventDiff {
    std::size_t queued = 0;    // events accepted by the queue (after coalescing)
    std::size_t structure = 0; // StructureChanged events emitted
    std::size_t properties = 0;
    bool focus_changed = false;
    bool nodes_added = false;
    bool nodes_removed = false;
};

// Derives the UIA notifications for one publication: `previous` is the last published tree,
// `next` the tree being published. The diff is pre-order and deterministic: one root
// ChildrenInvalidated when the node set changed, one per node whose child list changed, property
// changes in a fixed order (name, value, description, enabled, selected, bounds), then one
// FocusChanged when the focused node moved. All events go through `queue` (coalesced).
[[nodiscard]] UiaEventDiff queue_snapshot_diff(const A11ySnapshot& previous,
                                               const A11ySnapshot& next, UiaEventQueue& queue);

} // namespace tessera::ui::a11y
