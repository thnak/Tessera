// SPDX-License-Identifier: MIT
//
// The app's window set (issue #180, ADR-050 section 2: "Windows register with it; each window gets
// a child service scope").
//
// **Why this exists.** AF3 landed `Application` with a single `WindowLifecycle` member, because one
// `WindowHost` was one window and the state machine only ever had one to fold. ADR-050 section 2
// derives every state from a phrase about the *whole set* - "a window focused", "all windows
// blurred", "no visible window" - so the single member was a one-window special case of the rule,
// not the rule. This header is the rule: a bounded registry of window aggregates plus the pure
// function that folds them into the one aggregate `derive_app_state` already consumes.
//
// **What it deliberately is not.** It owns no window, no device, no scene and no service scope. It
// is a table of five booleans per window and the aggregation over them, so it is provable without a
// window, a clock or a GPU - which is what lets the aggregation table be asserted row by row
// instead of only end to end through a real platform.

#pragma once

#include "tessera/appfw/lifecycle.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace tessera::appfw {

// A window's identity inside one `Application`. It is an opaque handle rather than an index into
// the registry: a detached window's id is never reused, so a stale id from a closed window can
// never address a window that was attached later.
struct WindowId {
    std::uint32_t value = 0;

    friend constexpr bool operator==(WindowId, WindowId) = default;
};

// The window the `<App>` subtree describes (startup-api-v1.md section 10). `Application` registers
// it at construction, so a single-window app never names a `WindowId` at all and every pre-#180
// caller keeps its exact behaviour.
inline constexpr WindowId kPrimaryWindow{0};

// How many windows one `Application` tracks. It bounds the registry's fixed arrays, so
// `attach_window` never allocates and the aggregate walk is a bounded loop. Four covers the desktop
// cases V1 names (a main window plus a detached panel, a dialog and a preview), the same bound
// `app::kMaxWindowsPerDevice` uses for the renderer's frame ring.
inline constexpr std::size_t kMaxWindowsPerApplication = 4;

// The ADR-050 section 2 aggregate: one `WindowLifecycle` describing *all* the app's windows, so
// `derive_app_state` stays the single rule for both the one-window and the N-window case.
//
// Each field is the ADR's own phrase, not a new concept:
//
//   `visible`   some window is on screen (not minimized, not occluded, not closed)
//   `focused`   some *visible* window has focus - the ADR's "a window focused" row
//   `closed`    no window is open at all - the ADR's "no visible window" row for a tray app
//   `minimized` no window is visible and at least one is still open
//
// `occluded` is always false: occlusion and minimization are the same row of the derivation
// ("no visible window"), so reporting both would be two names for one fact. `minimized` carries it
// because that is the field `derive_app_state` already reads.
//
// An empty set is `closed`: an app with no window has no visible window, which is exactly the
// tray-app row (`quitOnLastWindowClosed = false`).
[[nodiscard]] constexpr WindowLifecycle
aggregate_window_lifecycle(std::span<const WindowLifecycle> windows) noexcept {
    WindowLifecycle aggregate{};
    aggregate.visible = false;
    aggregate.focused = false;
    aggregate.occluded = false;
    bool any_open = false;
    for (const WindowLifecycle& window : windows) {
        if (!window.closed) {
            any_open = true;
        }
        // A closed window is not visible even if its stale flags still say so: the platform may
        // report a focus for a window the app has already closed.
        const bool visible =
            window.visible && !window.minimized && !window.occluded && !window.closed;
        if (!visible) {
            continue;
        }
        aggregate.visible = true;
        aggregate.focused = aggregate.focused || window.focused;
    }
    aggregate.closed = !any_open;
    aggregate.minimized = !aggregate.visible && !aggregate.closed;
    return aggregate;
}

// The app's window set. Fixed capacity, no allocation after construction, and every operation is a
// bounded loop over at most `kMaxWindowsPerApplication` entries.
//
// Threading: UI thread only, exactly like the state machine that owns it.
class WindowRegistry {
public:
    explicit WindowRegistry(std::size_t capacity = kMaxWindowsPerApplication);
    ~WindowRegistry();

    WindowRegistry(const WindowRegistry&) = delete;
    WindowRegistry& operator=(const WindowRegistry&) = delete;

    // Registers a window and returns its id, or nullopt when the table is full. Ids are never
    // reused, so a caller that kept an id from a detached window cannot address a later one.
    std::optional<WindowId> attach(WindowLifecycle initial = {});

    // Removes a window. False when the id is unknown (already detached, or never attached).
    bool detach(WindowId id);

    // Folds one lifecycle signal into one window's aggregate, with the same rules
    // `fold_window_lifecycle` applies to a single window. False when the id is unknown.
    bool fold(WindowId id, LifecycleSignal signal);

    // Replaces one window's visibility and focus from a platform snapshot, with the same rules
    // `Application::apply_window_snapshot` applies to a single window. False when the id is
    // unknown.
    bool apply_snapshot(WindowId id, bool focused, bool occluded, bool minimized);

    [[nodiscard]] const WindowLifecycle* find(WindowId id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return count_ == 0;
    }
    // How many registered windows are still open (not closed). This is the count
    // `quitOnLastWindowClosed` is about: closing the last *open* window ends the app.
    [[nodiscard]] std::size_t open_count() const noexcept;

    // The ADR's aggregate over every registered window.
    [[nodiscard]] WindowLifecycle aggregate() const noexcept {
        return aggregate_window_lifecycle(windows());
    }

    // The registered windows and their ids, in attach order, for a caller that walks them.
    [[nodiscard]] std::span<const WindowLifecycle> windows() const noexcept {
        return std::span<const WindowLifecycle>(lifecycles_.get(), count_);
    }
    [[nodiscard]] std::span<const WindowId> ids() const noexcept {
        return std::span<const WindowId>(ids_.get(), count_);
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

private:
    // The slot index of `id`, or `count_` when it is not registered.
    [[nodiscard]] std::size_t index_of(WindowId id) const noexcept;

    // Two parallel arrays rather than an array of pairs, so `windows()` and `ids()` are both a
    // contiguous span and neither has to be rebuilt to be read.
    std::unique_ptr<WindowId[]> ids_;
    std::unique_ptr<WindowLifecycle[]> lifecycles_;
    std::size_t capacity_ = 0;
    std::size_t count_ = 0;
    // The next id to hand out. Monotonic and never reset, which is what makes a detached window's
    // id stale rather than recycled.
    std::uint32_t next_id_ = 0;
};

} // namespace tessera::appfw
