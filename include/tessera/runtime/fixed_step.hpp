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
