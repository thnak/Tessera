// SPDX-License-Identifier: MIT
//
// The window driver's lifecycle seam (ADR-050 section 1 rule 1: layer 0 gains the seam, layer 1
// owns the concept; AF3 of the roadmap).
//
// The driver already folds normalized `hal::WindowEvent` values into its own state: the occlusion
// pause, the accessibility snapshot, the pacer. Before this file it dropped the transitions above
// that fold, so a `Focus`, `Blur`, `Minimized`, `Occluded`, `Restored` or `Close` reached the
// driver and nothing else - which is why ADR-050's context records "no app object or app
// lifecycle". This translation unit is the whole forwarding surface: one borrowed
// `app::LifecycleSink*`, fed in poll order from the event fold and from the metrics fold, and
// nothing here names an app state, a service or a store. A hand-written `main()` that installs no
// sink keeps today's behaviour exactly (ADR-050 section 1 rule 5).
//
// It lives in its own file so `window_lifecycle.cpp` (the pause state machine and the bounded
// single-iteration seam) stays inside its size budget.

#include "window_impl.hpp"

namespace tessera::app {

// The baselines a fresh window starts from, so its first snapshot is not reported as a transition:
// a window the platform created visible and focused is already `Active`.
void WindowHost::Impl::seed_lifecycle_baseline(const hal::WindowMetrics& metrics) {
    lifecycle_baseline = lifecycle_baseline_for(metrics);
}

// Forwards one lifecycle transition to the installed sink. Called from `handle_window_event` for
// each event it folds, in poll order, and after the driver's own pause state has been updated, so a
// sink that reads the window sees the same state the driver just committed.
void WindowHost::Impl::forward_lifecycle(const hal::WindowEvent& event) const {
    if (lifecycle_sink == nullptr) {
        return;
    }
    lifecycle_sink->on_window_lifecycle(event);
}

// The metrics half of the pause machine. Occlusion has no Win32/X11 event yet, so the snapshot's
// `occluded`/`minimized` flags are the channel a platform - and the deterministic test seam -
// reports the lifecycle through; they own the metrics side of the pause state
// (docs/design/window-resize.md section 8) and they are what the layer above is told about, because
// ADR-050 section 2's desktop source names the window's visibility and focus, not an event type.
//
// The caller (`WindowHost::Impl::apply_metrics`, which owns the swapchain and the relayout) runs
// this before its presentation half, so a snapshot that hides the window never resizes a target at
// 0x0.
void WindowHost::Impl::apply_metrics_lifecycle(const hal::WindowMetrics& metrics) {
    metrics_paused_ = pause_policy.pauses(metrics.occluded, metrics.minimized);
    update_pause_state();
    forward_metrics_lifecycle(metrics);
    // Accessibility bounds follow the client area, so a metrics snapshot is a semantics change even
    // when the scene itself has no dirty flags.
    if (accessibility != nullptr) {
        accessibility->mark_dirty();
    }
}

// Only the transitions the snapshot actually moved are forwarded, in the ADR's precedence order
// (visibility first, then focus), which is what `snapshot_lifecycle_events` decides. A
// snapshot reporting the window visible again is forwarded as `Restored`, the HAL's own vocabulary
// for "shown again" (`WindowEventType::Restored` maps to `OsEventType::WindowShown`); a hidden
// window is forwarded as `Occluded` or `Minimized`, occlusion winning when a platform reports both.
void WindowHost::Impl::forward_metrics_lifecycle(const hal::WindowMetrics& metrics) {
    if (lifecycle_sink == nullptr) {
        return;
    }
    hal::WindowEventType types[2]{};
    const std::size_t count = snapshot_lifecycle_events(metrics, lifecycle_baseline, types);
    hal::WindowEvent event;
    event.metrics = metrics;
    for (std::size_t index = 0; index < count; ++index) {
        event.type = types[index];
        lifecycle_sink->on_window_lifecycle(event);
    }
}

} // namespace tessera::app
