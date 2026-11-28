// SPDX-License-Identifier: MIT

#include "web_frame_scheduler.hpp"

namespace tessera::hal::web {

bool FrameCallbackScheduler::wake() {
    if (!visible_) {
        pending_while_hidden_ = true;
        return false;
    }
    if (outstanding_) {
        return false;
    }
    outstanding_ = true;
    return true;
}

void FrameCallbackScheduler::begin_callback() {
    outstanding_ = false;
}

FrameDecision FrameCallbackScheduler::end_callback(const runtime::RuntimeTick& tick,
                                                   double timer_wait_seconds) {
    FrameDecision decision;
    if (!visible_) {
        return decision;
    }
    if (tick.device_blocked) {
        decision.wake_after_seconds = kDeviceBlockedRetrySeconds;
        return decision;
    }
    const bool timer_due = timer_wait_seconds <= 0.0;
    if (!tick.idle || tick.completions_pending || timer_due) {
        outstanding_ = true;
        decision.request_frame = true;
        return decision;
    }
    // Parked: the only thing that may wake the page without input is a timer.
    decision.wake_after_seconds = timer_wait_seconds;
    return decision;
}

bool FrameCallbackScheduler::set_visible(bool visible) {
    const bool was_visible = visible_;
    visible_ = visible;
    if (!visible) {
        return false;
    }
    if (was_visible && !pending_while_hidden_) {
        return false;
    }
    pending_while_hidden_ = false;
    if (outstanding_) {
        return false;
    }
    outstanding_ = true;
    return true;
}

} // namespace tessera::hal::web
