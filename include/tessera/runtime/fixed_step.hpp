// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::runtime {

struct FixedStepConfig {
    double fixed_timestep_hz = 60.0;
    double max_frame_delta_ms = 250.0;
    std::uint32_t max_updates_per_frame = 5;
    double time_scale = 1.0;
};

struct FixedStepResult {
    std::uint32_t steps = 0;
    double alpha = 0.0;
    double accumulated_seconds = 0.0;
    bool dropped_backlog = false;
};

// Fraction of one fixed step that has elapsed since the last simulation tick, in [0, 1]. This is
// the render interpolation alpha: the paint pass blends the previous and current tick samples with
// it. It is 0 when a render coincides with a tick (the accumulator was just drained), and
// approaches 1 as the next tick comes due. Non-finite or non-positive inputs yield 0 rather than a
// value a renderer could use to extrapolate: a broken clock must freeze visual interpolation, not
// fling it.
[[nodiscard]] double interpolation_alpha(double accumulated_seconds, double fixed_dt_seconds);

class FixedStepLoop {
public:
    explicit FixedStepLoop(FixedStepConfig config = {});

    FixedStepResult advance(double frame_dt_seconds);
    void reset();
    void set_time_scale(double scale);

    [[nodiscard]] double fixed_dt() const {
        return fixed_dt_;
    }
    [[nodiscard]] double accumulator() const {
        return accumulator_;
    }
    [[nodiscard]] double alpha() const;
    [[nodiscard]] const FixedStepConfig& config() const {
        return config_;
    }

private:
    FixedStepConfig config_;
    double fixed_dt_ = 1.0 / 60.0;
    double accumulator_ = 0.0;
};

} // namespace tessera::runtime
