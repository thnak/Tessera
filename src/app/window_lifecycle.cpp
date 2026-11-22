// SPDX-License-Identifier: MIT
//
// Occluded/background pause for the app window driver (docs/design/window-resize.md section 8).
//
// A window the platform reports as occluded or minimized must present nothing: acquiring an image
// the user cannot see keeps the GPU and the display pipeline busy and is exactly the work ADR-019
// forbids. This file owns the pause state machine (platform lifecycle events plus the latest
// WindowMetrics), the resume edge that latches a full repaint, and the bounded single-iteration
// seam (WindowHost::pump_once) that makes both deterministic in tests. It never waits on the GPU:
// a paused iteration parks on the same bounded platform wait as an idle one.

#include "tessera/app/idle_policy.hpp"
#include "tessera/ui/scene/node.hpp"
#include "window_cursor.hpp"
#include "window_impl.hpp"

#include <array>
#include <cmath>
#include <cstddef>

namespace tessera::app {
namespace {

namespace hal = tessera::hal;

bool subtree_dirty(const ui::Node& root) {
    return root.subtree_dirty() != ui::Dirty::Clean || root.dirty() != ui::Dirty::Clean;
}

} // namespace

// The lifecycle seam (`forward_lifecycle`, `apply_metrics_lifecycle`, `forward_metrics_lifecycle`
// and the baselines they compare against) lives in window_lifecycle_seam.cpp, so this file stays
// the pause state machine and the bounded single-iteration seam. The calls below are the whole
// coupling.

// Folds one normalized platform event into the driver state. Events never run user code and never
// touch the GPU; a lifecycle event only moves the pause state.
bool WindowHost::Impl::handle_window_event(const hal::WindowEvent& event) {
    switch (event.type) {
    case hal::WindowEventType::Close:
        forward_lifecycle(event);
        return true;
    case hal::WindowEventType::Resize:
    case hal::WindowEventType::DpiChange:
    case hal::WindowEventType::ScaleChanged:
        apply_metrics(window->metrics());
        // The client area moved, so every snapshot bound is stale. `apply_metrics` marks the
        // controller too; the explicit mark keeps the accessibility tree honest even when the
        // platform reports a resize that does not go through the metrics path (a DPI-only change).
        if (accessibility != nullptr) {
            accessibility->mark_dirty();
        }
        break;
    // Display-capability events (docs/specs/os-events-v1.md section 2, ADR-023 rule 4): the new
    // size/scale/refresh/work area is in the platform's metrics, so one snapshot push carries it.
    // `apply_metrics` re-lays out, resizes the swapchain and marks the render dirty; the pacer also
    // follows the reported refresh at this tick's frame boundary (apply_refresh_rate).
    case hal::WindowEventType::RefreshRateChanged:
        apply_metrics(window->metrics());
        apply_refresh_rate();
        break;
    case hal::WindowEventType::WorkAreaChanged:
    case hal::WindowEventType::DisplayChanged:
    case hal::WindowEventType::DisplayAdded:
    case hal::WindowEventType::DisplayRemoved:
        // A topology/work-area change does not move the client area by itself, but it does change
        // the display caps every layout and pacing decision reads, so the host takes the snapshot
        // and the loop re-evaluates media queries at this tick.
        apply_metrics(window->metrics());
        apply_refresh_rate();
        display_topology_changed = true;
        break;
    case hal::WindowEventType::SafeAreaChanged:
    case hal::WindowEventType::ImeChanged:
    case hal::WindowEventType::FoldChanged:
        // Insets, the keyboard and the hinge do not move the client area: no swapchain resize, only
        // a relayout of the SafeArea nodes, which read the new insets.
        host->set_window_metrics(window->metrics());
        loop->mark_render_dirty();
        break;
    case hal::WindowEventType::Minimized:
        // The window is not visible: the configured pause policy decides whether a frame may be
        // acquired, painted or presented until the platform reports it back. Win32 emits Minimized;
        // Occluded is the normalized value a platform with occlusion support (and the synthetic
        // seams) report.
        lifecycle_paused_ = pause_policy.pause_when_minimized;
        update_pause_state();
        forward_lifecycle(event);
        break;
    case hal::WindowEventType::Occluded:
        lifecycle_paused_ = pause_policy.pause_when_occluded;
        update_pause_state();
        forward_lifecycle(event);
        break;
    case hal::WindowEventType::Restored: {
        // The HAL's visible/restore transition (there is no separate Visible value). It clears the
        // event-driven pause, and the live metrics decide the rest: a window restored into a still
        // occluded state stays paused.
        lifecycle_paused_ = false;
        const hal::WindowMetrics current = window->metrics();
        metrics_paused_ = pause_policy.pauses(current.occluded, current.minimized);
        update_pause_state();
        // The forwarded event carries the snapshot the fold above just read, so the layer above
        // derives the same answer this driver did rather than a second guess at it.
        hal::WindowEvent restored = event;
        restored.metrics = current;
        forward_lifecycle(restored);
        break;
    }
    case hal::WindowEventType::Focus:
    case hal::WindowEventType::Blur:
        // Focus never pauses: blur only reports that another window owns the keyboard, and a
        // visible background window keeps painting (docs/design/performance.md section 5). The
        // accessibility tree exposes the focused node, so a focus change publishes a new snapshot.
        if (accessibility != nullptr) {
            accessibility->mark_dirty();
        }
        forward_lifecycle(event);
        break;
    case hal::WindowEventType::SystemSettingsChanged:
        // The platform re-reads its settings when it sees the broadcast, so one normal read here is
        // enough for both preferences: the reduced-motion toggle and a color-scheme change travel
        // through the same snapshot (issue #187). A change reaches the host once, and the media
        // queries are re-resolved by `set_appearance` when the color scheme moved.
        apply_appearance();
        break;
    case hal::WindowEventType::ThemeChanged:
        // The Windows immersive-color set / the Linux portal's SettingChanged became this event.
        apply_appearance();
        break;
    }
    return false;
}

// The appearance half of the settings path (issue #187, docs/specs/os-events-v1.md sections 2 and
// 5). The platform event carries no value, so the host re-reads the window's normalized snapshot at
// this tick boundary -- the same rule the pre-existing reduced-motion path followed, now covering
// the color scheme too. Both preferences travel together: `AppHost::set_appearance` applies the
// motion flag to the animation runtime and re-resolves the stylesheet when the color scheme moved.
// A re-read that did not move is a no-op, and the render is woken only for a real change so a
// static scene stays idle (ADR-019).
void WindowHost::Impl::apply_appearance() {
    if (window == nullptr) {
        return;
    }
    const hal::AppearancePreference observed = window->appearance();
    // Windows keeps its SPI query for a backend whose window does not implement the HAL snapshot;
    // the color scheme then stays assumed, which is what a platform without a source reports.
    const bool reduced = observed.reduced_motion || system_prefers_reduced_motion();
    hal::AppearancePreference next = observed;
    next.reduced_motion = reduced;
    if (motion.update(reduced)) {
        host->set_reduced_motion(motion.reduced());
        loop->mark_render_dirty();
    }
    // The color scheme is compared through the host's own snapshot, so a broadcast for an unrelated
    // setting (same theme) costs one comparison and no style rebuild.
    host->set_appearance(next);
}

// The display refresh the platform reports now reaches the pacer through its existing rate-change
// path (docs/specs/performance.md section 6, step 3; docs/specs/os-events-v1.md section 11). A
// refresh event is the OS saying the divisor base moved - a mode switch, a move onto a 120 Hz
// monitor - so the present cap must re-derive from it at this tick's frame boundary instead of
// keeping the creation-time value. An assumed 60 Hz (a platform that cannot report one) never
// overrides an explicit `WindowOptions::refresh_hz`: `resolve_display_refresh_hz` already encoded
// that precedence at creation, so this follows the same rule.
void WindowHost::Impl::apply_refresh_rate() {
    if (!available || window == nullptr) {
        return;
    }
    const hal::WindowMetrics metrics = window->metrics();
    // The event says the display caps are stale; the metrics carry the resolved value and its
    // assumption flag, so an unmeasured 60 Hz never silently replaces a measured rate.
    const double reported =
        metrics.assumed.refresh_hz ? options.refresh_hz : static_cast<double>(metrics.refresh_hz);
    const double resolved = hal::resolve_display_refresh_hz(
        options.refresh_hz, static_cast<double>(metrics.refresh_hz));
    // An explicit option still wins, so only a resolution that actually moved rewrites the pacer.
    const double next = resolved > 0.0 ? resolved : reported;
    if (!std::isfinite(next) || next <= 0.0 || next == options.refresh_hz) {
        return;
    }
    options.refresh_hz = next;
    // The pacer owns the deadline; its rate-change path resyncs to one interval after now, so the
    // new refresh takes effect at the next frame boundary with no burst and no swapchain recreate.
    pacer->update(runtime::FramePacerConfig{applied_present_cap_hz, options.refresh_hz, false},
                  hal::steady_now_seconds());
    ++stats.refresh_rate_changes;
    // Media queries and rem units derive from the display caps, so the style environment changed.
    loop->mark_render_dirty();
}

// The display topology re-read at a topology event's tick boundary (section 3: a `DisplayAdded`/
// `DisplayRemoved`/`DisplayChanged` says the list is stale; the loop re-reads it once, not once per
// event, so a hotplug storm costs one query). A backend with no per-monitor source reports zero
// displays, which is the documented answer, not a fabricated monitor.
bool WindowHost::Impl::sync_display_topology() {
    if (!display_topology_changed) {
        return false;
    }
    display_topology_changed = false;
    if (!available || window == nullptr) {
        return false;
    }
    const std::size_t count = window->displays(displays_);
    const auto next_count =
        static_cast<std::uint8_t>(count > displays_.size() ? displays_.size() : count);
    bool changed = next_count != display_count_;
    if (!changed) {
        for (std::size_t index = 0; index < next_count; ++index) {
            // A monitor's id is its identity, so a different id at any index is a topology change
            // even when the count stayed the same (one monitor replaced by another).
            if (displays_[index].id != last_displays_[index].id) {
                changed = true;
                break;
            }
        }
    }
    last_displays_ = displays_;
    display_count_ = next_count;
    stats.display_count = display_count_;
    if (!changed) {
        return false;
    }
    // The display list feeds resolution/media-query inputs, so the style environment is stale.
    loop->mark_render_dirty();
    return true;
}

// Step 3 of the design order: the controller's decided present cap reaches the pacer through the
// pacer's existing rate-change path. `update()` resyncs the next deadline to one new interval after
// `now_seconds`, so the change lands at the next frame boundary - no burst of catch-up frames, no
// stall at the old cadence, no blocking wait and no swapchain recreation. A cap that did not move
// skips the update() (the running deadline stays untouched) but still refreshes the report.
void WindowHost::Impl::apply_present_cap(double now_seconds) {
    const double decided = host->adaptive_quality().present_cap_hz;
    if (decided != applied_present_cap_hz) {
        applied_present_cap_hz = decided;
        pacer->update(runtime::FramePacerConfig{decided, options.refresh_hz, false}, now_seconds);
        ++stats.present_cap_changes;
    }
    stats.present_cap_hz = decided;
    stats.present_interval_seconds = pacer->interval_seconds();
}

void WindowHost::Impl::pump_input() {
    // Display topology at this iteration's tick boundary (docs/specs/os-events-v1.md section 3): a
    // `DisplayAdded`/`DisplayRemoved`/`DisplayChanged` event from the drain above only marks the
    // list stale; it is re-read here, once per tick, never inside the platform callback. Both
    // driver paths call this immediately after their event drain, so the boundary is the same.
    (void)sync_display_topology();
    const ui::InputPumpStats pumped = pump.pump(root, *loop);
    stats.input_events += static_cast<std::uint32_t>(pumped.consumed);
    if (subtree_dirty(root)) {
        loop->mark_render_dirty();
        // Input changes focus, hover, selection and text values: the semantics snapshot is stale
        // whenever a dispatched event dirtied the scene.
        if (accessibility != nullptr) {
            accessibility->mark_dirty();
        }
    }
    // Pointer shape right after dispatch (issue #191): hover was just updated, so the cursor
    // request and the `:hover` style can never disagree. A static hover makes no platform call
    // (apply_cursor compares against `last_cursor`), so this costs a pointer compare per iteration.
    sync_cursor();
}

// The app half of the CSS cursor seam (docs/specs/window-chrome-v1.md section 8.5, issue #191): the
// deepest hovered node's resolved cursor kind is mapped onto the HAL's shape and requested from the
// platform. `cursor` is inherited, so the node value already carries the ancestor resolution.
//
// A platform without a pointer-shape source is reported once (the same one-shot policy as
// full-screen) and the app keeps running: a missing cursor never fails a frame or a run.
void WindowHost::Impl::sync_cursor() {
    if (!available || window == nullptr) {
        return;
    }
    const hal::CursorShape wanted = detail::hovered_cursor(pump.router().hovered());
    (void)detail::apply_cursor(*window, wanted, last_cursor, cursor_unsupported_reported);
}

void WindowHost::Impl::apply_initial_auto_focus() {
    ui::FocusManager& focus = pump.router().focus_manager();
    focus.set_root(root);
    // `focusTrap` scopes follow the tree every frame; opening or closing one moves focus in or
    // restores it (issue #236).
    focus.sync_focus_trap(root);
    if (auto_focus_applied) {
        return;
    }
    auto_focus_applied = true;
    if (focus.apply_auto_focus(root)) {
        loop->mark_render_dirty();
    }
}

void WindowHost::Impl::update_pause_state() {
    const bool next = lifecycle_paused_ || metrics_paused_;
    if (next == paused_) {
        return;
    }
    paused_ = next;
    if (next) {
        // Nothing painted since the pause began can be trusted to describe the screen, and the next
        // resume must not trust the pre-pause damage either; the resume edge latches that repaint.
        return;
    }
    // Resume: the damage/redraw state describes the pre-pause scene, and a paused window may have
    // missed a platform invalidation (a compositor discard, a display or DPI switch). A full-target
    // repaint is the only safe first frame; the request also wakes the on-demand loop.
    host->request_full_repaint();
}

// Host-owned caret blink (docs/specs/text-editing-v1.md section 12). The blink is the driver's, not
// paint's: every iteration of run() and pump_once() advances it once, against the configured clock,
// after the pause guard. An inactive blink is never ticked, so an unfocused field costs no frame
// request and no GPU work; a paused window never ticks either (the pause wins over the blink).
void WindowHost::Impl::tick_caret() {
    if (!caret.active()) {
        return;
    }
    if (!caret.tick(caret_now_seconds())) {
        return;
    }
    ++stats.caret_blinks;
    // The caret is not a scene node, so the damage plan cannot place it. Mark the loop dirty and
    // latch a full-target repaint - the driver's existing "something the damage state does not
    // describe changed" lever, the same one the pause resume uses - so this flip gets exactly one
    // frame. A repeated tick in the same phase asks for nothing.
    loop->mark_render_dirty();
    host->request_full_repaint();
}

void WindowHost::Impl::set_caret_active(bool active) {
    const bool changed = caret.set_active(active, caret_now_seconds());
    if (!changed || host == nullptr || !available) {
        // Deactivating hides the caret (caret_visible() reads false immediately) and requests
        // nothing: the field that lost focus is the one that repaints its own focus state, and an
        // inactive blink stays idle across any number of clock advances.
        return;
    }
    // Showing the caret is a visible change: request the frame that paints it.
    loop->mark_render_dirty();
    if (active) {
        host->request_full_repaint();
    }
}

// One bounded iteration: pump events and input, honour the pause, tick the loop and produce at most
// one frame. Deliberately no parking, retry or device-loss servicing - run() owns those, and a
// caller of this seam decides when the next iteration happens.
//
// `advance_pacer` is the multi-window half (issue #180). The pacer is app-scoped: one app presents
// at one rate (docs/design/multi-window.md section 5). That makes it a gate on *ticks*, not on
// windows - so a group walk that drove N windows must advance it once, after the last window, not
// once per window. Advancing it per window would let the first window in the walk consume the
// interval and starve every sibling for as long as the walk order stayed the same, which is exactly
// the "independent pacing" the acceptance forbids.
bool WindowHost::Impl::pump_once(bool advance_pacer) {
    if (!available) {
        return false;
    }
    // The single-window path ticks the shared loop itself, exactly as it did before #180.
    return pump_once(loop->tick(), advance_pacer);
}

bool WindowHost::Impl::pump_once(const runtime::RuntimeTick& tick, bool advance_pacer) {
    if (!available) {
        return false;
    }
    stats.available = true;
    if (stats.device_state == WindowDeviceState::Unavailable) {
        stats.device_state = WindowDeviceState::Ready;
    }
    // A previous run() detached the queue at exit so a platform event could not outlive its pump;
    // automation reattaches it so a posted event is seen by this iteration.
    window->set_input_queue(&pump.queue());

    std::array<hal::WindowEvent, detail::kMaxWindowEvents> events{};
    const std::size_t event_count = window->poll_events(events);
    bool closed = false;
    for (std::size_t index = 0; index < event_count; ++index) {
        closed = handle_window_event(events[index]) || closed;
    }
    if (closed) {
        // The window asked to close: this seam does not own shutdown, so it produces nothing.
        return false;
    }

    pump_input();
    if (paused()) {
        // Input is still drained (the loop must not go deaf while paused) but no frame is begun:
        // no acquire, no paint, no present, no animation tick.
        ++stats.paused_iterations;
        last_tick_seconds_ = hal::steady_now_seconds();
        return false;
    }

    // Caret tick, between the pause guard and the frame decision: an inactive blink is skipped
    // entirely and a flip marks the frame dirty for this iteration (single-iteration twin of the
    // call in run()).
    tick_caret();

    if (stats.frames == 0 || subtree_dirty(root)) {
        // Startup is a frame cause, exactly like run(): the scene has no dirty flags until the
        // first layout, and the first iteration of an automation script is expected to paint.
        loop->mark_render_dirty();
    }

    if (tick.device_blocked || tick.device_failed || tick.device_recovered || !tick.should_render) {
        // No device-loss servicing and no forced frames here: a lost device makes this iteration a
        // no-op, and run() remains the bounded recovery path.
        return false;
    }
    const double now = hal::steady_now_seconds();
    if (!pacer->ready(now).render_now) {
        // The present-rate cap is due later; the caller decides whether to wait for it.
        return false;
    }

    const double dt = now - last_tick_seconds_;
    last_tick_seconds_ = now;
    const HostFrame frame = host->begin_frame(dt, tick.alpha);
    apply_initial_auto_focus();
    tick_accessibility(&frame.tick);
    if (!frame.frame_due || frame.render.redraw_plan.scissors.empty()) {
        // A wake that changed nothing: clear the dirt this tick collected so it cannot stay latched
        // into a phantom render request, and present nothing.
        host->end_frame();
        loop->clear_render_dirty();
        return false;
    }

    const std::uint32_t presented = stats.presented;
    const FrameResult outcome = render_and_present(frame.render.full_redraw);
    if (outcome != FrameResult::Presented) {
        // Same rule as run(): the attempt consumed the repaint request and the damage plan, so
        // latch the whole target again or the retry would present nothing. A busy slot is
        // backpressure rather than a fault, but it still painted nothing, so it needs the same
        // relatch.
        if (outcome == FrameResult::Skipped || outcome == FrameResult::Busy) {
            host->request_full_repaint();
        }
        return false;
    }
    // The pacer's deadline is advanced once per *tick*, not once per window: a group walk passes
    // `advance_pacer == false` and the group advances it after the last window (issue #180). A
    // single-window caller passes true and keeps the pre-#180 behaviour exactly.
    if (advance_pacer) {
        pacer->present(hal::steady_now_seconds());
    }
    // Same one-frame adaptive-quality observation as run(): `now` is this frame's start (the
    // pacer check above returned without waiting); the source is the GPU interval when the RHI
    // probe reported it usable and a completed submission produced one, else this frame's CPU cost.
    (void)observe_frame_time((hal::steady_now_seconds() - now) * 1000.0);
    // Step 3: the decided present cap reaches the pacer at this frame boundary (single-iteration
    // twin of the call in run()).
    apply_present_cap(hal::steady_now_seconds());
    host->end_frame();
    loop->clear_render_dirty();
    return stats.presented > presented;
}

} // namespace tessera::app
