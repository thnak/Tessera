// SPDX-License-Identifier: MIT

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/system_clock.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <type_traits>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "hal_timer: " << message << "\n";
    }
    return condition;
}

bool near(double value, double expected, double tolerance = 1.0e-12) {
    return std::abs(value - expected) <= tolerance;
}

} // namespace

int main() {
    using tessera::hal::seconds_until;
    using tessera::hal::steady_now_seconds;
    using tessera::hal::wait_until_seconds;
    using tessera::runtime::Clock;
    using tessera::runtime::SystemClock;

    if (!expect(near(seconds_until(5.0, 1.0), 0.0), "past deadline clamps to zero")) {
        return 1;
    }
    if (!expect(near(seconds_until(2.0, 2.0), 0.0), "equal deadline yields zero")) {
        return 2;
    }
    if (!expect(near(seconds_until(1.0, 2.5), 1.5), "positive remaining time")) {
        return 3;
    }
    if (!expect(near(seconds_until(-5.0, -2.0), 3.0), "negative now stays positive")) {
        return 4;
    }
    if (!expect(near(seconds_until(-2.0, 0.0), 2.0), "negative now to zero deadline")) {
        return 5;
    }

    const double before_wait = steady_now_seconds();
    const auto wait_start = std::chrono::steady_clock::now();
    wait_until_seconds(before_wait - 10.0);
    const auto wait_elapsed = std::chrono::steady_clock::now() - wait_start;
    const double waited = std::chrono::duration<double>(wait_elapsed).count();
    if (!expect(waited < 1.0, "past deadline returns promptly")) {
        return 6;
    }

    double previous = steady_now_seconds();
    for (int i = 0; i < 64; ++i) {
        const double current = steady_now_seconds();
        if (!expect(std::isfinite(current), "steady time is finite")) {
            return 7;
        }
        if (!expect(current >= previous, "steady time is non-decreasing")) {
            return 8;
        }
        previous = current;
    }

    static_assert(std::is_base_of_v<Clock, SystemClock>, "SystemClock must implement Clock");

    SystemClock system_clock;
    const Clock& clock = system_clock;
    double system_previous = clock.now_seconds();
    if (!expect(std::isfinite(system_previous), "system clock is finite")) {
        return 9;
    }
    for (int i = 0; i < 64; ++i) {
        const double current = clock.now_seconds();
        if (!expect(std::isfinite(current), "system clock remains finite")) {
            return 10;
        }
        if (!expect(current >= system_previous, "system clock is non-decreasing")) {
            return 11;
        }
        system_previous = current;
    }

    std::cout << "hal_timer: ok\n";
    return 0;
}
