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
                       full_frame};
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

bool RuntimeLoop::should_render() const {
    if (continuous_ || !config_.render_on_demand) {
        return true;
    }
    return render_dirty_ || frame_requested_;
}

} // namespace tessera::runtime
