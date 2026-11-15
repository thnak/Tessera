// SPDX-License-Identifier: MIT

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/runtime_loop.hpp"

namespace tessera::runtime {

RuntimeLoop::RuntimeLoop(Clock& clock, RuntimeLoopConfig config)
    : clock_(&clock),
      config_(config),
      fixed_step_(config.fixed_step) {}

RuntimeTick RuntimeLoop::tick() {
    // D7: poll the device-loss controller once per tick. It makes at most one bounded recovery
    // attempt and reports outcomes as values; the loop itself never blocks here.
    DeviceRecoveryUpdate recovery;
    const bool has_recovery = config_.device_recovery != nullptr;
    if (has_recovery) {
        recovery = config_.device_recovery->tick();
    }
    const bool blocked = has_recovery && config_.device_recovery->recovery_required();

    const double now = clock_->now_seconds();
    const double delta = started_ ? now - last_time_ : 0.0;
    started_ = true;
    last_time_ = now;

    std::size_t completions = 0;
    if (config_.completion_queue != nullptr) {
        completions = config_.completion_queue->run(config_.completion_budget);
    }

    // Timers fire after the completion drain and before the fixed step, so work they schedule is
    // drained by the next tick and the step this tick advances sees the timer's effects. A timer
    // never sets `frame_requested_`; it is a wake source, not a render cause (ADR-050 section 5
    // item 2). The clock instant read above is the one used, so a ManualClock test and a real clock
    // agree on what "due" means.
    std::size_t timers_fired = 0;
    if (config_.timers != nullptr) {
        timers_fired = config_.timers->fire_due(now);
    }

    const FixedStepResult step = fixed_step_.advance(delta);

    if (recovery.full_frame_required) {
        // The device generation changed: damage/redraw state is invalid, so the next painted frame
        // must be full. This is a one-shot request (it does not latch scene dirty state).
        frame_requested_ = true;
        full_frame_pending_ = true;
    }

    bool render = should_render();
    if (recovery.recovered) {
        // Give the caller the recovery tick to recreate the swapchain and RHI resources from the
        // new generation; the next tick paints the full frame.
        render = false;
    }
    if (blocked) {
        // No presents while lost/recovering. Drop the queued request so callers idle on the
        // bounded platform/pacer wait instead of spinning ticks.
        render = false;
        frame_requested_ = false;
    }
    const bool full_frame = full_frame_pending_;
    if (render) {
        frame_requested_ = false;
        full_frame_pending_ = false;
    }

    const bool pending = config_.completion_queue != nullptr && !config_.completion_queue->empty();
    if (pending && !blocked) {
        frame_requested_ = true;
    }

    return RuntimeTick{step.steps,
                       step.alpha,
                       render,
                       step.dropped_backlog,
                       completions,
                       pending,
                       blocked,
                       recovery.state,
                       recovery.recovered,
                       recovery.failed,
                       recovery.generation,
                       !render && !recovery.recovered,
                       full_frame,
                       timers_fired};
}

void RuntimeLoop::request_frame() {
    frame_requested_ = true;
}

void RuntimeLoop::mark_render_dirty() {
    render_dirty_ = true;
}

void RuntimeLoop::clear_render_dirty() {
    render_dirty_ = false;
}

void RuntimeLoop::reset() {
    fixed_step_.reset();
    last_time_ = clock_->now_seconds();
    started_ = false;
    continuous_ = false;
    render_dirty_ = false;
    frame_requested_ = false;
    full_frame_pending_ = false;
}

void RuntimeLoop::set_render_on_demand(bool enabled) {
    config_.render_on_demand = enabled;
}

void RuntimeLoop::set_continuous(bool enabled) {
    continuous_ = enabled;
}

void RuntimeLoop::set_completion_queue(CompletionQueue* queue) {
    config_.completion_queue = queue;
}

void RuntimeLoop::set_completion_budget(std::size_t budget) {
    config_.completion_budget = budget;
}

void RuntimeLoop::set_device_recovery(DeviceRecoveryController* controller) {
    config_.device_recovery = controller;
}

void RuntimeLoop::set_timer_queue(TimerQueue* timers) {
    config_.timers = timers;
}

double RuntimeLoop::next_timer_wait_seconds() const {
    if (config_.timers == nullptr) {
        return kNoDeadline;
    }
    const double deadline = config_.timers->next_deadline_seconds();
    if (!(deadline < kNoDeadline)) {
        return kNoDeadline;
    }
    const double wait = deadline - clock_->now_seconds();
    // A due (or overdue) timer asks for zero: the host must not sleep past it, and a negative wait
    // would read as "park" to `idle_wait_seconds`.
    return wait > 0.0 ? wait : 0.0;
}

bool RuntimeLoop::should_render() const {
    if (continuous_ || !config_.render_on_demand) {
        return true;
    }
    return render_dirty_ || frame_requested_;
}

} // namespace tessera::runtime
