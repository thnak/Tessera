// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/runtime/clock.hpp"

namespace tessera::runtime {

// The production Clock: the HAL monotonic clock, the same one FramePacer and the window wait use.
// Header-only so it adds no link dependency; tests keep using ManualClock.
class SteadyClock final : public Clock {
public:
    [[nodiscard]] double now_seconds() const override {
        return hal::steady_now_seconds();
    }
};

} // namespace tessera::runtime
