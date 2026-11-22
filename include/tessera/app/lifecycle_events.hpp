// SPDX-License-Identifier: MIT
//
// Which lifecycle transitions a window snapshot implies (ADR-050 section 2's desktop source, AF3).
//
// The driver reports the lifecycle two ways. A platform with an event channel forwards
// `hal::WindowEvent` values; a platform without one - Win32 and X11 have no occlusion event -
// reports the same thing through `hal::WindowMetrics`, which `WindowHost::Impl::apply_metrics`
// already folds into its own pause state. The layer above must not have to know which channel a
// platform uses, so the snapshot is translated into the same event vocabulary, once per real
// transition.
//
// This header is the rule itself, separated from the forwarding so it is a pure function: the tests
// prove the precedence (occlusion outranks minimization, focus is independent) without a window, a
// device or a GPU.

#pragma once

#include "tessera/hal/window.hpp"

#include <cstddef>

namespace tessera::app {

// The baseline a window starts from, so its first snapshot is not reported as a transition.
struct LifecycleBaseline {
    bool hidden = false;
    bool focused = true;

    friend constexpr bool operator==(const LifecycleBaseline&, const LifecycleBaseline&) = default;
};

// The baseline a window's creation snapshot implies. A window created visible and focused has
// nothing to report.
//
// Creation-time visibility is deliberately not part of this rule (issue #181). A window created
// with `WindowOptions::visible = false` is the offscreen/CI pattern - the app paints into a window
// the user never sees - so it is not a lifecycle transition and must not be reported as one. The
// tray app's hidden state is the app's own `WindowHost::set_visible(false)`, which is a driver
// decision rather than a platform snapshot, so it never reaches this function.
[[nodiscard]] constexpr LifecycleBaseline
lifecycle_baseline_for(const hal::WindowMetrics& metrics) noexcept {
    return LifecycleBaseline{metrics.occluded || metrics.minimized, metrics.focused};
}

// At most two events per snapshot, in the ADR's precedence order:
//
//   1. visibility - `Occluded` (which outranks `Minimized` when a platform reports both, because
//   the
//      user cannot see the window either way and the ADR derives `Hidden` from both) or `Restored`;
//   2. focus - `Focus` or `Blur`, independent of visibility.
//
// `out` must have room for two. Returns the count written (0 when the snapshot moved nothing), so a
// caller forwards exactly what changed and a repeated snapshot costs no dispatch.
[[nodiscard]] constexpr std::size_t snapshot_lifecycle_events(const hal::WindowMetrics& metrics,
                                                              LifecycleBaseline& baseline,
                                                              hal::WindowEventType* out) noexcept {
    std::size_t count = 0;
    const bool hidden = metrics.occluded || metrics.minimized;
    if (hidden != baseline.hidden) {
        baseline.hidden = hidden;
        out[count++] = hidden ? (metrics.occluded ? hal::WindowEventType::Occluded
                                                  : hal::WindowEventType::Minimized)
                              : hal::WindowEventType::Restored;
    }
    if (metrics.focused != baseline.focused) {
        baseline.focused = metrics.focused;
        out[count++] = metrics.focused ? hal::WindowEventType::Focus : hal::WindowEventType::Blur;
    }
    return count;
}

} // namespace tessera::app
