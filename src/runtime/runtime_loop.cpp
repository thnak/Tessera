// SPDX-License-Identifier: MIT

#include "tessera/runtime/runtime_loop.hpp"

namespace tessera::runtime {

RuntimeLoop::RuntimeLoop(Clock& clock, RuntimeLoopConfig config)
    : clock_(&clock),
      config_(config),
      fixed_step_(config.fixed_step) {}

RuntimeTick RuntimeLoop::tick() {
    const double now = clock_->now_seconds();
    const double delta = started_ ? now - last_time_ : 0.0;
    started_ = true;
    last_time_ = now;

    const FixedStepResult step = fixed_step_.advance(delta);
    const bool render = should_render();
    if (render) {
        frame_requested_ = false;
    }

    return RuntimeTick{step.steps, step.alpha, render, step.dropped_backlog};
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
}

void RuntimeLoop::set_render_on_demand(bool enabled) {
    config_.render_on_demand = enabled;
}

void RuntimeLoop::set_continuous(bool enabled) {
    continuous_ = enabled;
}

bool RuntimeLoop::should_render() const {
    if (continuous_ || !config_.render_on_demand) {
        return true;
    }
    return render_dirty_ || frame_requested_;
}

} // namespace tessera::runtime
