// SPDX-License-Identifier: MIT

#include "tessera/runtime/fixed_step.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tessera::runtime {
namespace {

constexpr double kEpsilon = 1.0e-12;

bool is_valid_seconds(double value) {
    return std::isfinite(value) && value >= 0.0;
}

} // namespace

FixedStepLoop::FixedStepLoop(FixedStepConfig config)
    : config_(config) {
    if (!std::isfinite(config_.fixed_timestep_hz) || config_.fixed_timestep_hz <= 0.0) {
        throw std::invalid_argument("fixed_timestep_hz must be finite and greater than zero");
    }
    if (!is_valid_seconds(config_.max_frame_delta_ms)) {
        throw std::invalid_argument("max_frame_delta_ms must be finite and non-negative");
    }
    if (config_.max_updates_per_frame == 0) {
        throw std::invalid_argument("max_updates_per_frame must be greater than zero");
    }
    if (!is_valid_seconds(config_.time_scale)) {
        throw std::invalid_argument("time_scale must be finite and non-negative");
    }

    fixed_dt_ = 1.0 / config_.fixed_timestep_hz;
}

FixedStepResult FixedStepLoop::advance(double frame_dt_seconds) {
    double frame_dt = frame_dt_seconds;
    if (!std::isfinite(frame_dt) || frame_dt < 0.0) {
        frame_dt = 0.0;
    }

    frame_dt = std::min(frame_dt, config_.max_frame_delta_ms / 1000.0);
    frame_dt *= config_.time_scale;
    accumulator_ += frame_dt;

    std::uint32_t steps = 0;
    while (accumulator_ + kEpsilon >= fixed_dt_ && steps < config_.max_updates_per_frame) {
        accumulator_ -= fixed_dt_;
        accumulator_ = std::max(accumulator_, 0.0);
        ++steps;
    }

    bool dropped_backlog = false;
    if (steps == config_.max_updates_per_frame && accumulator_ + kEpsilon >= fixed_dt_) {
        dropped_backlog = true;
        accumulator_ = std::min(accumulator_, fixed_dt_);
    }

    return FixedStepResult{steps, alpha(), accumulator_, dropped_backlog};
}

void FixedStepLoop::reset() {
    accumulator_ = 0.0;
}

void FixedStepLoop::set_time_scale(double scale) {
    if (!is_valid_seconds(scale)) {
        throw std::invalid_argument("time_scale must be finite and non-negative");
    }
    config_.time_scale = scale;
}

double FixedStepLoop::alpha() const {
    return std::clamp(accumulator_ / fixed_dt_, 0.0, 1.0);
}

} // namespace tessera::runtime
