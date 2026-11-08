// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window_wait.hpp"
#include "tessera/runtime/frame_pacer.hpp"

namespace tessera::runtime {

struct FrameWaitResult {
    bool event = false;
    bool deadline = false;
};

// Idles between ticks on the window event queue until the pacer's next deadline. Callers should
// render when the pacer says the frame is ready; this helper never spins and never blocks past the
// deadline (the HAL wait slices to the remaining time).
[[nodiscard]] inline FrameWaitResult wait_for_next_frame(hal::Window& window, FramePacer& pacer) {
    const double now = hal::steady_now_seconds();
    const PaceDecision decision = pacer.ready(now);
    if (decision.render_now || decision.wait_seconds <= 0.0) {
        return FrameWaitResult{false, true};
    }
    const hal::WaitOutcome outcome =
        hal::wait_for_events_until(window, now + decision.wait_seconds);
    FrameWaitResult result;
    result.event = outcome == hal::WaitOutcome::Event;
    result.deadline = outcome == hal::WaitOutcome::Deadline;
    return result;
}

} // namespace tessera::runtime
