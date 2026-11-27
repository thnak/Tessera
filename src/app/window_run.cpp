// SPDX-License-Identifier: MIT
//
// The window driver's blocking run loop (issue #228): the paced loop that parks on the pacer
// deadline, services device loss, and presents only when the scene is dirty. Split out of
// `window.cpp` when the multi-window refactor pushed that file past its size budget.
//
// The loop itself is the group's (`WindowDevice::loop()`), not this window's: one tick drives every
// window in the group, and this function is the per-window half that decides whether *this* window
// paints. That split is what lets two windows share a device and a pacer without sharing a scene.

#include "tessera/app/frame_guard.hpp"
#include "tessera/app/idle_policy.hpp"
#include "tessera/app/recovery_policy.hpp"
#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window_wait.hpp"
#include "window_impl.hpp"

#include <cstdint>

namespace tessera::app {

namespace hal = tessera::hal;
WindowFrameStats WindowHost::Impl::run(std::uint32_t max_frames) {
    stats = {};
    if (!available) {
        return stats;
    }
    stats.available = true;
    stats.device_state = WindowDeviceState::Ready;
    // The platform's resolved backdrop is creation-time state, not a per-frame query.
    const hal::WindowMetrics window_metrics = window->metrics();
    stats.backdrop = window_metrics.backdrop;
    stats.transparent = window_metrics.transparent;
    const bool force_frames = max_frames > 0;
    // A previous run detached the queue so a platform event could not outlive its pump, so a second
    // run() reattaches it on entry instead of leaving the window deaf forever.
    window->set_input_queue(&pump.queue());
    pacer->reset();
    // The attempt budget bounds one loop invocation: a caller that retries run() after a give-up
    // starts a fresh bounded recovery sequence (app/recovery_policy.hpp).
    RecoveryPolicy recovery_policy;

    bool closed = false;
    // The OS's modal resize loop stops this loop for as long as an edge is being dragged; the
    // platform calls back per size change so the content follows the frame. Removed on every exit
    // path below (the guard), because the callback captures this.
    window->set_live_frame_callback([this]() { live_resize_frame(); });
    struct LiveFrameGuard {
        hal::Window* window;
        ~LiveFrameGuard() {
            window->set_live_frame_callback({});
        }
    } live_frame_guard{window.get()};
    last_tick_seconds_ = hal::steady_now_seconds();
    // A frame that keeps failing (a device that never recovers, a swapchain that never yields an
    // image) stops the run instead of spinning (ADR-021). Backpressure is bounded separately: a
    // busy in-flight slot is expected under load (see FrameResult::Busy below).
    FrameFailureGuard failures;
    BusyRetryBudget busy;
    std::array<hal::WindowEvent, detail::kMaxWindowEvents> events{};
    // Startup is a frame cause: the scene has no dirty flags until the first layout.
    loop->mark_render_dirty();

    while (!closed) {
        const std::size_t event_count = window->poll_events(events);
        for (std::size_t index = 0; index < event_count; ++index) {
            closed = handle_window_event(events[index]) || closed;
        }
        if (closed) {
            break;
        }

        pump_input();
        if (force_frames && stats.frames < max_frames) {
            loop->mark_render_dirty();
        }

        // A paused window (occluded, minimized or hidden by the platform) produces no frame: no
        // acquire, submit or present and no style/layout/paint or animation tick. The loop keeps
        // pumping events and input and parks on the same bounded platform wait, so a close, a
        // resize/metrics change or a restore wakes it. The pause wins over the forced-frame path:
        // run(max_frames) waits for the window to become visible again or to close.
        if (paused()) {
            ++stats.paused_iterations;
            // Drop the paused wall time so the first frame after resume gets a normal dt instead
            // of the whole pause duration.
            last_tick_seconds_ = hal::steady_now_seconds();
            idle(idle_wait_seconds(pacer->ready(last_tick_seconds_)));
            continue;
        }

        // After the pause guard and before the frame decision: a paused window never ticks the
        // caret (pause wins over the blink), and a flip requests exactly one frame for this
        // iteration.
        tick_caret();

        const runtime::RuntimeTick tick = loop->tick();
        if (tick.device_blocked || tick.device_failed) {
            // No presents while the device is lost: release once, let the runtime's bounded attempt
            // run, and park on the platform wait between attempts (never a spin).
            if (!service_device_loss(tick, recovery_policy, failures)) {
                break;
            }
            continue;
        }
        if (tick.device_recovered) {
            if (!service_recovery(recovery_policy, failures)) {
                break;
            }
            continue;
        }
        if (!tick.should_render) {
            // Settled scene: do not begin a host frame at all, so no style, layout or paint work
            // runs. Park until the next scheduled present or a wake source. Accessibility still
            // gets its turn: the controller only builds when dirty, so a settled window does no
            // snapshot work here either, while an accessibility-only change (a focus/resize mark)
            // cannot be stranded behind a frame that never happens.
            tick_accessibility();
            ++stats.idle_waits;
            trim_idle_offscreen(hal::steady_now_seconds());
            idle(idle_wait_seconds(pacer->ready(hal::steady_now_seconds())));
            continue;
        }

        // A frame the runtime wants still respects the present-rate cap: idle to the pacer deadline
        // instead of rendering early and sleeping a fixed amount afterwards.
        const double now = hal::steady_now_seconds();
        const runtime::PaceDecision pace = pacer->ready(now);
        if (!pace.render_now) {
            ++stats.idle_waits;
            idle(pacer_wait_seconds(pace));
            continue;
        }

        const double dt = now - last_tick_seconds_;
        last_tick_seconds_ = now;
        const HostFrame frame = host->begin_frame(dt, tick.alpha);
        apply_initial_auto_focus();
        tick_accessibility(&frame.tick);
        const bool has_damage = !frame.render.redraw_plan.scissors.empty();
        if (!should_present(frame.frame_due, has_damage, force_frames)) {
            // A wake that changed nothing: clear the dirt the tick collected so it cannot stay
            // latched into a phantom render request, then idle without painting. The forced CI path
            // is the only caller allowed to paint an unchanged scene.
            host->end_frame();
            loop->clear_render_dirty();
            ++stats.idle_waits;
            idle(idle_wait_seconds(pacer->ready(hal::steady_now_seconds())));
            continue;
        }

        const FrameResult outcome = render_and_present(frame.render.full_redraw);
        if (outcome != FrameResult::Presented) {
            ++stats.idle_waits;
            if (charge_frame_attempt(outcome, failures, busy) == FrameRetry::Stop) {
                break;
            }
            // Retry on the bounded backoff: a busy slot frees on any GPU completion, a skipped one
            // on the next acquire.
            idle(kFrameRetryWaitSeconds);
            continue;
        }
        failures.succeeded();
        busy.succeeded();
        pacer->present(hal::steady_now_seconds());
        // Adaptive quality (docs/design/performance.md section 6): the one frame observation, fed
        // from the GPU interval of the most recent completed submission when the device's
        // timestamp probe reported it usable, else from this frame's CPU cost through present. A
        // skipped attempt returned above and is not an observation.
        (void)observe_frame_time((hal::steady_now_seconds() - now) * 1000.0);
        apply_present_cap(hal::steady_now_seconds());
        host->end_frame();
        loop->clear_render_dirty();
        if (force_frames && stats.frames >= max_frames) {
            break;
        }
        // Park until the next present is due (a zero budget only pumps the event queue when the
        // pacer is uncapped, so a continuous loop never spins on a timer).
        idle(pacer_wait_seconds(pacer->ready(hal::steady_now_seconds())));
    }

    window->set_input_queue(nullptr);
    release_in_flight(detail::kDrainBudgetSeconds);
    // Sampled after the drain, once per run: two syscalls, never on the frame path. Before the
    // swapchain is torn down, so the numbers are the steady state a user sees in Task Manager.
    stats.process_memory = diagnostics::sample_process_memory();
    if (device != nullptr) {
        const rhi::DeviceMemoryStats gpu = device->memory_statistics();
        stats.gpu_memory.buffer_bytes = gpu.buffer_bytes;
        stats.gpu_memory.image_bytes = gpu.image_bytes;
        stats.gpu_memory.block_bytes = gpu.block_bytes;
        stats.gpu_memory.allocation_bytes = gpu.allocation_bytes;
        for (std::size_t i = 0;
             i < gpu.categories.size() && i < diagnostics::kGpuMemoryCategoryCount; ++i) {
            stats.gpu_memory.category_bytes[i] = gpu.categories[i].live_bytes;
            stats.gpu_memory.category_peak_bytes[i] = gpu.categories[i].peak_bytes;
        }
    }
    return stats;
}

void WindowHost::Impl::live_resize_frame() {
    if (!available || paused()) {
        return;
    }
    std::array<hal::WindowEvent, detail::kMaxWindowEvents> events{};
    const std::size_t event_count = window->poll_events(events);
    for (std::size_t index = 0; index < event_count; ++index) {
        // A Close during a drag is left to the main loop: it re-reads the pending flag when the
        // modal loop returns, and tearing the window down from inside its own message is not safe.
        if (events[index].type == hal::WindowEventType::Close) {
            continue;
        }
        (void)handle_window_event(events[index]);
    }
    pump_input();
    loop->mark_render_dirty();
    const runtime::RuntimeTick tick = loop->tick();
    if (!tick.should_render) {
        return;
    }
    const double now = hal::steady_now_seconds();
    const double dt = now - last_tick_seconds_;
    last_tick_seconds_ = now;
    const HostFrame frame = host->begin_frame(dt, tick.alpha);
    if (render_and_present(true) != FrameResult::Presented) {
        // Busy or skipped: the next size message (or the main loop after the drag) retries.
        return;
    }
    pacer->present(hal::steady_now_seconds());
    host->end_frame();
    loop->clear_render_dirty();
}

} // namespace tessera::app
