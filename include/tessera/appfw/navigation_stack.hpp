// SPDX-License-Identifier: MIT
//
// The navigation stack and controller (docs/specs/navigation-v1.md sections 2, 3, 4, 6, 8 and 10.1;
// ADR-050 sections 6 and 8, row `navigation`; issue #196).
//
// Three decisions this header makes, because the spec fixes the shape and leaves the mechanism:
//
//   1. **Frame-boundary publish, and the route is an `Observable`.** Section 3: "Route changes are
//      frame-boundary operations; no navigation in render." A request mutates the stack and marks
//      it dirty; `commit()` publishes the new active entry through AF4's
//      `Observable<RouteSnapshot>` at the frame boundary, so a component hears about a route change
//      once per flush no matter how many pushes landed in the tick - AF4's coalescing applied to
//      navigation.
//   2. **Every operation returns a named result**, never `void` and never a throw. Section 3 puts
//   the
//      unknown route on the not-found screen; section 2 makes back-at-root an exit policy; section
//      10 makes every input untrusted. A caller that ignores a result still cannot crash, and a
//      test asserts the code instead of "it survived".
//   3. **The stack is fixed-capacity** (ADR-050 section 10: no hidden allocations on hot paths). A
//      push beyond `NavControllerConfig::capacity` is `CapacityExceeded`; a steady-state tick
//      allocates nothing, and `commit()` allocates only when the route actually changed.
//
// Back is section 10.1's canonical priority. This module owns steps 2, 3 and the *reporting* of 4:
// topmost dismissible overlay (not here - overlays belong to `ui::scene::OverlayHost`), then
// topmost modal/sheet entry, then a normal pop, then root/OS exit policy, which the host applies.
// `back()` returns which of those it did, so the host composes the overlay step in front of it.
//
// Deliberately not in this slice, named so a caller does not assume it: guard/auth evaluation
// (section 4 - `RouteDef::auth` is carried but no `GuardPipeline` runs it), route loaders and
// `async-data` (section 10.1 "Route data", G-13), transitions (section 7: "they do not change
// navigation semantics"), and per-tab stacks (Q-11: not in V1; one stack, id `main`).

#pragma once

#include "tessera/appfw/navigation_route.hpp"
#include "tessera/appfw/observable.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

// Section 10.1's `NavEntry`, minus its `TransitionSpec` (section 7: a transition is an animation,
// so it is renderer presentation state, never entry identity).
struct NavEntry {
    // "stable entry id", "stable for restore/automation" (section 10.1). Assigned monotonically
    // from a seed the snapshot carries, so a restored stack keeps its ids and an automation script
    // can name an entry across a restart.
    std::uint64_t entry_id = 0;
    std::string route_id;
    ParamMap params;
    StateMap
        state; // "serializable, opt-in restore" (section 10.1, G-13: UI state only, never data)
    Presentation presentation = Presentation::Page;

    [[nodiscard]] bool operator==(const NavEntry&) const = default;
};

// The published frame-boundary value: the active entry plus a monotonically increasing epoch. The
// epoch is what section 4's "stale results are discarded by navigation epoch" is checked against.
// It is deliberately *not* part of `operator==`: `Observable` compares what the frame will read, so
// a redundant request neither bumps the epoch nor wakes a subscriber.
struct RouteSnapshot {
    NavEntry entry;
    std::size_t depth = 0;
    std::uint64_t epoch = 0;
    bool at_root = true;

    [[nodiscard]] bool operator==(const RouteSnapshot& other) const noexcept {
        return entry.entry_id == other.entry.entry_id && entry.route_id == other.entry.route_id &&
               entry.params == other.entry.params && depth == other.depth &&
               at_root == other.at_root;
    }
};

// Section 2's duplicate policy is per route (`RouteDef::duplicates`); this is the controller-level
// bound a table row should not have to repeat.
struct NavControllerConfig {
    // Bound on the stack depth. Section 2 gives no number; 64 is a desktop back stack that never
    // needs a scroll, and it makes the bound explicit instead of unbounded growth from an untrusted
    // link.
    std::size_t capacity = 64;
};

// What `back()` did, so the host composes section 10.1's four-step priority without re-deriving it.
enum class BackOutcome : std::uint8_t {
    // A normal entry was popped (section 10.1 item 3). The previous entry is now active.
    Popped,
    // A modal or sheet entry was popped (section 10.1 item 2). Reported separately because the host
    // restores focus to whatever opened it, and because a test distinguishes the two cases.
    DismissedModal,
    // Nothing left above the root entry. Section 2: "Back at root: exit/close per platform policy."
    // This module does not exit, close or quit - it reports the edge and the host applies its
    // policy
    // (window close, no-op, or an unsaved-work confirmation).
    AtRoot,
    // The stack was empty when back was requested. Defined behaviour, not undefined: the stack
    // never
    // underflows, and `Empty` is reachable only before the first `commit()` of a stack-less
    // controller.
    Empty,
};

[[nodiscard]] constexpr std::string_view back_outcome_name(BackOutcome outcome) noexcept {
    switch (outcome) {
    case BackOutcome::Popped:
        return "popped";
    case BackOutcome::DismissedModal:
        return "dismissed-modal";
    case BackOutcome::AtRoot:
        return "at-root";
    case BackOutcome::Empty:
        return "empty";
    }
    return "empty";
}

// What `push`/`replace` did, which is more than the error code: section 2's duplicate policy turns
// a push into either a no-op or a replace, and both are successes.
enum class PushOutcome : std::uint8_t {
    // A new entry was added (or the active entry was replaced).
    Committed,
    // Section 2's `Ignore` duplicate policy: the same route with the same params was already
    // active, so
    // the request focused it instead of growing the stack.
    DuplicateIgnored,
};

// Bounded result of a `back()` call.
struct BackResult {
    BackOutcome outcome = BackOutcome::Empty;
    // The entry that is active after the call, or null when the stack is now empty.
    std::uint64_t active_entry = 0;
};

// Section 10.1's `NavController`: "active stack, push/pop/replace, epoch, transitions", and
// explicitly "does not own Overlay/modal dismissal policy". One per window (Q-11: "a per-instance
// handle, never a global").
class NavController {
public:
    // `table` is borrowed and must outlive the controller: it is static app data. `scheduler` is
    // the window's, because the route observable attaches a ticket to it.
    NavController(const RouteTable& table, ui::UpdateScheduler& scheduler,
                  NavControllerConfig config = {});
    ~NavController();

    NavController(const NavController&) = delete;
    NavController& operator=(const NavController&) = delete;
    NavController(NavController&&) = delete;
    NavController& operator=(NavController&&) = delete;

    // --- requests ------------------------------------------------------------------------------
    //
    // A request validates against the table before it records anything, so a bad path leaves the
    // stack exactly as it was. The change becomes visible at `commit()`.

    // Section 2's `push`. An unknown path is `NotFound` (section 3's not-found screen is the
    // caller's mapping, because which route is "not found" is table data).
    [[nodiscard]] std::expected<PushOutcome, NavigationError> push(std::string_view path);
    [[nodiscard]] std::expected<PushOutcome, NavigationError> push_route(std::string_view route_id,
                                                                         ParamMap params = {});
    // Section 2's `replace`: the active entry is replaced and history does not grow.
    [[nodiscard]] std::expected<PushOutcome, NavigationError> replace(std::string_view path);
    [[nodiscard]] std::expected<PushOutcome, NavigationError>
    replace_route(std::string_view route_id, ParamMap params = {});

    // Section 2/10.1's back, in the priority order this module owns. Never fails: every outcome is
    // a defined behaviour, because "back with nothing to pop" is a normal user action, not a bug.
    [[nodiscard]] BackResult back();

    // Section 2's `pop_to`: removes every entry above `entry_id`. Popping to the active entry is a
    // successful no-op; an entry that is not in the stack is `UnknownEntry` and changes nothing.
    [[nodiscard]] std::expected<void, NavigationError> pop_to(std::uint64_t entry_id);

    // Section 2's `reset` / `replace_all`: history becomes exactly `entries`, with `active_index`
    // active. Used by `reset_to` and by a restore, so a restored stack is validated by the same
    // code a navigation is. An empty list is `EmptyStack` (section 2: "reset clears it" means "to
    // the root", not "to nothing" - see `reset_to`).
    [[nodiscard]] std::expected<void, NavigationError> replace_all(std::vector<NavEntry> entries,
                                                                   std::size_t active_index);

    // Section 2's `reset`, expressed the way a caller wants it: clear history and install the route
    // `path` resolves to.
    [[nodiscard]] std::expected<void, NavigationError> reset_to(std::string_view path);

    // --- per-entry state (section 8, G-13) ------------------------------------------------------

    // Section 10.1: `NavEntry.state` is "serializable, opt-in restore". The controller carries it
    // and assigns it to a live entry; it never interprets the values. False when the entry is
    // unknown.
    bool set_entry_state(std::uint64_t entry_id, StateMap state);
    [[nodiscard]] const StateMap* entry_state(std::uint64_t entry_id) const noexcept;

    // --- the frame boundary ---------------------------------------------------------------------

    // Publishes the pending route through the observable. Returns true when the published value
    // changed. A tick with no request returns false before constructing anything, so a steady-state
    // frame allocates nothing.
    bool commit();

    [[nodiscard]] bool pending() const noexcept {
        return dirty_;
    }
    [[nodiscard]] const Observable<RouteSnapshot>& route() const noexcept {
        return route_;
    }
    [[nodiscard]] Observable<RouteSnapshot>& route() noexcept {
        return route_;
    }

    // The working stack, bottom first. A request is visible here immediately (the stack is the
    // request's result); what is frame-boundary is the *published* route.
    [[nodiscard]] const std::vector<NavEntry>& stack() const noexcept {
        return entries_;
    }
    [[nodiscard]] std::size_t depth() const noexcept {
        return entries_.size();
    }
    [[nodiscard]] const NavEntry* active() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept {
        return epoch_;
    }
    [[nodiscard]] const RouteTable& table() const noexcept {
        return *table_;
    }

private:
    // Matches `path` into an entry without touching the stack, and validates its typed params.
    [[nodiscard]] std::expected<NavEntry, NavigationError> entry_for_path(std::string_view path);
    [[nodiscard]] std::expected<NavEntry, NavigationError>
    entry_for_route(std::string_view route_id, ParamMap params);

    // The one place a request becomes stack state: the duplicate policy (section 2), the stack
    // bound, and the replace-vs-push difference all live here so
    // `push`/`replace`/`push_route`/`replace_route` cannot disagree about them.
    [[nodiscard]] std::expected<PushOutcome, NavigationError> record(NavEntry entry,
                                                                     bool replace_top);

    // Section 2: same route id and same params as the active entry.
    [[nodiscard]] bool is_duplicate(const NavEntry& candidate) const;

    // Marks the stack dirty and rebuilds nothing: the snapshot is built in `commit()` from the then
    // active entry, so N requests in one tick cost N descriptions, not N snapshots.
    void touch() noexcept {
        dirty_ = true;
    }

    [[nodiscard]] std::uint64_t next_entry_id() noexcept {
        return ++last_entry_id_;
    }

    const RouteTable* table_ = nullptr;
    NavControllerConfig config_;
    std::vector<NavEntry> entries_;
    Observable<RouteSnapshot> route_;
    std::uint64_t epoch_ = 0;
    std::uint64_t last_entry_id_ = 0;
    bool dirty_ = false;
};

} // namespace tessera::appfw
