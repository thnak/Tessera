// SPDX-License-Identifier: MIT

#pragma once

// Full-screen bookkeeping shared by the platform backends (docs/specs/window-chrome-v1.md
// section 13, issue #232).
//
// The OS calls differ per platform (Win32 saves and reapplies a `WINDOWPLACEMENT`; X11 asks the
// window manager for `_NET_WM_STATE_FULLSCREEN` and lets it restore), but the bookkeeping between
// them is identical and is what the round-trip contract is about: remember the bounds the window had
// before entering, report "restore these bounds" exactly once on exit, and treat a repeated request
// as a no-op. Keeping that here means both backends share one tested state machine and the pure
// rules can be verified without a display.

#include "tessera/hal/geometry.hpp"

namespace tessera::hal {

struct FullscreenState {
    // The state the platform last resolved, not the last request: a window manager that refuses a
    // state change leaves this false.
    bool active = false;
    // Outer window bounds (position and size) to go back to on exit, captured when entering.
    WindowRect restore_bounds;
    bool restore_valid = false;
};

// Decides what entering full-screen means for `state`, given the window's current outer bounds.
// Returns true when the caller must perform the platform enter call: false for a repeated request
// (already active), which keeps the recorded bounds from being overwritten by the full-screen ones.
// A non-finite or empty rectangle is not recorded, so a backend that cannot answer leaves the
// restore path explicitly "no bounds to restore" instead of remembering zeros.
[[nodiscard]] bool fullscreen_enter(FullscreenState& state, const WindowRect& current_bounds) noexcept;

// Decides what leaving full-screen means. Returns true when the caller must perform the platform
// exit call; `state.restore_bounds` was zeroed by this call, so a backend that wants to restore saved
// bounds takes a copy *before* calling. A request while not active is a no-op, so an app cannot push
// a stale restore over a user's resize. On a true return the saved bounds are consumed: the next
// enter records fresh ones.
[[nodiscard]] bool fullscreen_exit(FullscreenState& state) noexcept;

// The bounds `fullscreen_exit` found saved, if any. Callers pass the state they are about to exit
// (before the exit consumes it), which keeps "did the window have bounds to return to" readable.
[[nodiscard]] bool fullscreen_has_restore_bounds(const FullscreenState& state) noexcept;

// Folds the state the platform actually resolved into `state` (a user shortcut or the window
// manager can toggle full-screen outside `set_fullscreen`). A resolved value different from the
// recorded one updates the flag but never invents restore bounds: only `fullscreen_enter` records
// them, because only it knows the pre-entry rectangle. Returns the resolved value.
[[nodiscard]] bool fullscreen_resolved(FullscreenState& state, bool resolved) noexcept;

// True when `bounds` can be restored to: finite and with a positive size. Position may be zero.
[[nodiscard]] bool fullscreen_restorable(const WindowRect& bounds) noexcept;

} // namespace tessera::hal
