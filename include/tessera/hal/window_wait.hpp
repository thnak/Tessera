// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"

namespace tessera::hal {

enum class WaitOutcome {
    Event,
    Deadline,
};

// Idles until `window` has at least one event queued or `deadline_seconds` is reached.
//
// The clock is `steady_now_seconds()`. Each platform wait is a slice bounded by the remaining time
// (and kMaxWaitSliceSeconds), so a wakeup cannot overshoot the deadline and an early non-event
// wakeup is re-checked. Event is returned as soon as the window reports one. A deadline in the
// past, or a non-finite deadline, returns Deadline without waiting. Must be called on the window's
// owning thread.
[[nodiscard]] inline WaitOutcome wait_for_events_until(Window& window, double deadline_seconds) {
    constexpr double kMaxWaitSliceSeconds = 0.05;
    for (;;) {
        const double now = steady_now_seconds();
        if (!(deadline_seconds > now)) {
            return WaitOutcome::Deadline;
        }
        const double remaining = deadline_seconds - now;
        const double slice = remaining < kMaxWaitSliceSeconds ? remaining : kMaxWaitSliceSeconds;
        if (window.wait_for_events(slice)) {
            return WaitOutcome::Event;
        }
    }
}

} // namespace tessera::hal
