// SPDX-License-Identifier: MIT
//
// The full-screen state machine both window backends share (issue #232).
//
// The OS mechanism is per platform, the contract is not: enter once, remember the bounds the window
// had, restore exactly those on exit, never overwrite them with a repeated request, and follow a
// state the user changed outside the app. That contract is what the headless test checks, so it
// lives here rather than being duplicated in two platform files where only one is covered by the
// local lane.

#include "tessera/hal/fullscreen.hpp"

#include <cmath>

namespace tessera::hal {

bool fullscreen_restorable(const WindowRect& bounds) noexcept {
    return std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.width) &&
           std::isfinite(bounds.height) && bounds.width > 0.0f && bounds.height > 0.0f;
}

bool fullscreen_enter(FullscreenState& state, const WindowRect& current_bounds) noexcept {
    if (state.active) {
        // Already full-screen: the current bounds are the full-screen ones, so recording them would
        // make the exit restore inside the full-screen rectangle.
        return false;
    }
    state.active = true;
    if (fullscreen_restorable(current_bounds)) {
        state.restore_bounds = current_bounds;
        state.restore_valid = true;
    } else {
        // No usable rectangle to return to: the exit falls back to the platform's own idea of the
        // window's normal placement. It stays explicit instead of remembering zeros.
        state.restore_bounds = WindowRect{};
        state.restore_valid = false;
    }
    return true;
}

bool fullscreen_has_restore_bounds(const FullscreenState& state) noexcept {
    return state.restore_valid && fullscreen_restorable(state.restore_bounds);
}

bool fullscreen_exit(FullscreenState& state) noexcept {
    if (!state.active) {
        return false;
    }
    state.active = false;
    // The saved bounds are consumed: a second exit cannot replay them over a resize the user made
    // after leaving, and the next enter records fresh ones. A backend restores `restore_bounds`
    // when it is valid and otherwise falls back to its own idea of the window's normal placement.
    state.restore_bounds = WindowRect{};
    state.restore_valid = false;
    return true;
}

bool fullscreen_resolved(FullscreenState& state, bool resolved) noexcept {
    if (state.active != resolved) {
        state.active = resolved;
        if (!resolved) {
            // Full-screen ended outside set_fullscreen (an OS shortcut, a window manager): whatever
            // bounds the platform restored are now the truth, so a stale record must not survive.
            state.restore_bounds = WindowRect{};
            state.restore_valid = false;
        }
    }
    return state.active;
}

} // namespace tessera::hal
