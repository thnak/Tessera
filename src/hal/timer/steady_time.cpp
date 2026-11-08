// SPDX-License-Identifier: MIT

#include "tessera/hal/timer/steady_time.hpp"

#include <chrono>
#include <cmath>
#include <thread>

namespace tessera::hal {

double steady_now_seconds() noexcept {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(elapsed).count();
}

double seconds_until(double now_seconds, double deadline_seconds) noexcept {
    if (!(deadline_seconds > now_seconds)) {
        return 0.0;
    }
    return deadline_seconds - now_seconds;
}

void wait_until_seconds(double deadline_seconds) {
    const double remaining = seconds_until(steady_now_seconds(), deadline_seconds);
    if (!std::isfinite(remaining) || remaining <= 0.0) {
        return;
    }
    // A single sleep is bounded by the deadline and does not spin; the UI thread
    // may idle here between ticks (ADR-021). Early wakeups from input/completions
    // arrive with the B3 platform event wait, not this portable primitive.
    std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
}

} // namespace tessera::hal
