// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/fixed_step.hpp"

#include <cstdint>

namespace tessera::runtime {

struct RuntimeLoopConfig {
    FixedStepConfig fixed_step;
    bool render_on_demand = true;
};

struct RuntimeTick {
    std::uint32_t steps = 0;
    double alpha = 0.0;
    bool should_render = false;
    bool dropped_backlog = false;
};

class RuntimeLoop {
public:
    explicit RuntimeLoop(Clock& clock, RuntimeLoopConfig config = {});

    RuntimeTick tick();
    void request_frame();
    void mark_render_dirty();
    void clear_render_dirty();
    void reset();

    void set_render_on_demand(bool enabled);
    void set_continuous(bool enabled);

    [[nodiscard]] bool should_render() const;
    [[nodiscard]] bool render_on_demand() const {
        return config_.render_on_demand;
    }
    [[nodiscard]] bool continuous() const {
        return continuous_;
    }
    [[nodiscard]] const FixedStepLoop& fixed_step() const {
        return fixed_step_;
    }

private:
    Clock* clock_ = nullptr;
    RuntimeLoopConfig config_;
    FixedStepLoop fixed_step_;
    double last_time_ = 0.0;
    bool started_ = false;
    bool continuous_ = false;
    bool render_dirty_ = false;
    bool frame_requested_ = false;
};

} // namespace tessera::runtime
