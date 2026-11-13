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
#include "window_impl.hpp"

#include <array>
#include <cstddef>

namespace tessera::app {
namespace {

namespace hal = tessera::hal;

bool subtree_dirty(const ui::Node& root) {
    return root.subtree_dirty() != ui::Dirty::Clean || root.dirty() != ui::Dirty::Clean;
}

} // namespace

// Folds one normalized platform event into the driver state. Events never run user code and never
// touch the GPU; a lifecycle event only moves the pause state.
bool WindowHost::Impl::handle_window_event(const hal::WindowEvent& event) {
    switch (event.type) {
    case hal::WindowEventType::Close:
        return true;
    case hal::WindowEventType::Resize:
    case hal::WindowEventType::DpiChange:
        apply_metrics(window->metrics());
        break;
    case hal::WindowEventType::Minimized:
        // The window is not visible: the configured pause policy decides whether a frame may be
        // acquired, painted or presented until the platform reports it back. Win32 emits Minimized;
        // Occluded is the normalized value a platform with occlusion support (and the synthetic
        // seams) report.
        lifecycle_paused_ = pause_policy.pause_when_minimized;
        update_pause_state();
        break;
    case hal::WindowEventType::Occluded:
        lifecycle_paused_ = pause_policy.pause_when_occluded;
        update_pause_state();
        break;
    case hal::WindowEventType::Restored: {
        // The HAL's visible/restore transition (there is no separate Visible value). It clears the
        // event-driven pause, and the live metrics decide the rest: a window restored into a still
        // occluded state stays paused.
        lifecycle_paused_ = false;
        const hal::WindowMetrics current = window->metrics();
        metrics_paused_ = pause_policy.pauses(current.occluded, current.minimized);
        update_pause_state();
        break;
    }
    case hal::WindowEventType::Focus:
    case hal::WindowEventType::Blur:
        // Focus never pauses: blur only reports that another window owns the keyboard, and a
        // visible background window keeps painting (docs/design/performance.md section 5).
        break;
    case hal::WindowEventType::SystemSettingsChanged:
        if (motion.update(system_prefers_reduced_motion())) {
            host->set_reduced_motion(motion.reduced());
            // In-flight animations must finish (or snap) even if the scene was idle.
            loop->mark_render_dirty();
        }
        break;
    }
    return false;
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
        pacer.update(runtime::FramePacerConfig{decided, options.refresh_hz, false}, now_seconds);
        ++stats.present_cap_changes;
    }
    stats.present_cap_hz = decided;
    stats.present_interval_seconds = pacer.interval_seconds();
}

void WindowHost::Impl::pump_input() {
    const ui::InputPumpStats pumped = pump.pump(root, *loop);
    stats.input_events += static_cast<std::uint32_t>(pumped.consumed);
    if (subtree_dirty(root)) {
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
bool WindowHost::Impl::pump_once() {
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

    const runtime::RuntimeTick tick = loop->tick();
    if (tick.device_blocked || tick.device_failed || tick.device_recovered || !tick.should_render) {
        // No device-loss servicing and no forced frames here: a lost device makes this iteration a
        // no-op, and run() remains the bounded recovery path.
        return false;
    }
    const double now = hal::steady_now_seconds();
    if (!pacer.ready(now).render_now) {
        // The present-rate cap is due later; the caller decides whether to wait for it.
        return false;
    }

    const double dt = now - last_tick_seconds_;
    last_tick_seconds_ = now;
    const HostFrame frame = host->begin_frame(dt, tick.alpha);
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
        // Same rule as run(): the skipped frame consumed the repaint request and the damage plan,
        // so latch the whole target again or the retry would present nothing.
        if (outcome == FrameResult::Skipped) {
            host->request_full_repaint();
        }
        return false;
    }
    pacer.present(hal::steady_now_seconds());
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
