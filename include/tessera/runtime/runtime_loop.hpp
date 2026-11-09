// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/device_recovery.hpp"
#include "tessera/runtime/fixed_step.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::runtime {

class CompletionQueue;

struct RuntimeLoopConfig {
    FixedStepConfig fixed_step;
    bool render_on_demand = true;
    CompletionQueue* completion_queue = nullptr;
    std::size_t completion_budget = 64;
    // Optional D7 device-loss orchestration: when set, the loop polls it once per tick, never
    // presents while a recovery is required, and requests a full frame after a successful recovery.
    DeviceRecoveryController* device_recovery = nullptr;
};

struct RuntimeTick {
    std::uint32_t steps = 0;
    double alpha = 0.0;
    bool should_render = false;
    bool dropped_backlog = false;
    std::size_t completions = 0;
    bool completions_pending = false;
    // True while the device is lost/recovering: no frame may be presented this tick. The caller
    // must idle on the bounded platform wait (never spin) and release lost-generation resources.
    bool device_blocked = false;
    DeviceRecoveryState device_state = DeviceRecoveryState::Ready;
    // A recovery succeeded on this tick: recreate the swapchain/resources before the next render.
    bool device_recovered = false;
    // The one-shot recovery failed for this loss; stay stopped until a new loss.
    bool device_failed = false;
    // Bumps on every successful recovery; callers recreate resources when it changes.
    std::uint32_t device_generation = 0;
    // No frame is due this tick and the loop has no immediate work. Callers idle on the pacer
    // deadline / platform event wait instead of re-ticking immediately (no busy-wait). False on a
    // recovery tick: the caller recreates its resources then ticks again.
    bool idle = false;
    // True while a post-recovery full-frame repaint is pending: set on the recovery tick and on
    // the first render tick after it (the caller must paint the whole frame, damage state is
    // invalid), then cleared.
    bool full_frame = false;
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
    void set_completion_queue(CompletionQueue* queue);
    void set_completion_budget(std::size_t budget);
    // Attaches/removes the device-loss controller polled by tick() (D7).
    void set_device_recovery(DeviceRecoveryController* controller);

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
    [[nodiscard]] DeviceRecoveryController* device_recovery() const {
        return config_.device_recovery;
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
    bool full_frame_pending_ = false;
};

} // namespace tessera::runtime
