// SPDX-License-Identifier: MIT

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"

#include <cmath>

namespace tessera::hal {

// Platforms without a native window backend keep the seam linkable so shared code can query it.
std::unique_ptr<Window> create_window(const WindowDesc&) {
    return nullptr;
}

bool Window::wait_for_events(double timeout_seconds) {
    // No OS event source exists here, so idle for the bounded timeout and report no event. Callers
    // that wait between ticks still throttle instead of spinning.
    if (std::isfinite(timeout_seconds) && timeout_seconds > 0.0) {
        wait_until_seconds(steady_now_seconds() + timeout_seconds);
    }
    return false;
}

} // namespace tessera::hal
