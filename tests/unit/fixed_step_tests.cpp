// SPDX-License-Identifier: MIT

#include "tessera/runtime/fixed_step.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "fixed_step: " << message << "\n";
    }
    return condition;
}

bool near(double value, double expected, double tolerance = 1.0e-9) {
    return std::abs(value - expected) <= tolerance;
}

} // namespace

int main() {
    using tessera::runtime::FixedStepConfig;
    using tessera::runtime::FixedStepLoop;

    FixedStepLoop loop(FixedStepConfig{60.0, 250.0, 5, 1.0});
    if (!expect(near(loop.fixed_dt(), 1.0 / 60.0), "fixed dt")) {
        return 1;
    }

    auto result = loop.advance(1.0 / 60.0);
    if (!expect(result.steps == 1, "one step for one tick") ||
        !expect(near(result.alpha, 0.0), "alpha after exact tick") ||
        !expect(!result.dropped_backlog, "no backlog drop")) {
        return 2;
    }

    result = loop.advance(1.0 / 30.0);
    if (!expect(result.steps == 2, "two steps for two ticks") ||
        !expect(near(result.alpha, 0.0), "alpha after two ticks")) {
        return 3;
    }

    result = loop.advance(0.5);
    if (!expect(result.steps == 5, "max updates clamp") ||
        !expect(result.dropped_backlog, "backlog dropped") ||
        !expect(near(result.alpha, 1.0), "alpha after backlog clamp")) {
        return 4;
    }

    loop.reset();
    FixedStepLoop scaled(FixedStepConfig{60.0, 250.0, 5, 2.0});
    result = scaled.advance(1.0 / 120.0);
    if (!expect(result.steps == 1, "time scale doubles delta")) {
        return 5;
    }
    scaled.set_time_scale(0.0);
    result = scaled.advance(1.0 / 60.0);
    if (!expect(result.steps == 0, "zero time scale pauses simulation")) {
        return 6;
    }

    FixedStepLoop clamped(FixedStepConfig{60.0, 100.0, 100, 1.0});
    result = clamped.advance(1.0);
    if (!expect(result.steps == 6, "max frame delta clamps a long pause")) {
        return 7;
    }

    loop.reset();
    result = loop.advance(-1.0);
    if (!expect(result.steps == 0, "negative delta ignored")) {
        return 8;
    }
    result = loop.advance(std::numeric_limits<double>::quiet_NaN());
    if (!expect(result.steps == 0, "NaN delta ignored")) {
        return 9;
    }
    result = loop.advance(std::numeric_limits<double>::infinity());
    if (!expect(result.steps == 0, "infinite delta ignored")) {
        return 10;
    }

    bool threw = false;
    try {
        FixedStepLoop invalid(FixedStepConfig{0.0, 250.0, 5, 1.0});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "invalid frequency rejected")) {
        return 11;
    }

    threw = false;
    try {
        loop.set_time_scale(-1.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "invalid time scale rejected")) {
        return 12;
    }

    // Render interpolation alpha: the fraction of a fixed step elapsed since the last tick, so the
    // paint pass can blend the previous and current tick samples. 0 at a tick, 0.5 mid-step, 1 as
    // the next tick comes due, and clamped outside that range.
    using tessera::runtime::interpolation_alpha;
    const double dt = 1.0 / 60.0;
    if (!expect(near(interpolation_alpha(0.0, dt), 0.0), "alpha is 0 at a tick") ||
        !expect(near(interpolation_alpha(dt * 0.5, dt), 0.5), "alpha is 0.5 mid-step") ||
        !expect(near(interpolation_alpha(dt, dt), 1.0), "alpha is 1 at a full step") ||
        !expect(near(interpolation_alpha(dt * 3.0, dt), 1.0), "alpha clamps above 1") ||
        !expect(near(interpolation_alpha(-dt, dt), 0.0), "alpha clamps below 0")) {
        return 13;
    }
    if (!expect(near(interpolation_alpha(std::numeric_limits<double>::quiet_NaN(), dt), 0.0),
                "NaN accumulator freezes alpha") ||
        !expect(near(interpolation_alpha(std::numeric_limits<double>::infinity(), dt), 0.0),
                "infinite accumulator freezes alpha") ||
        !expect(near(interpolation_alpha(dt, 0.0), 0.0), "zero fixed dt yields 0") ||
        !expect(near(interpolation_alpha(dt, -dt), 0.0), "negative fixed dt yields 0") ||
        !expect(near(interpolation_alpha(dt, std::numeric_limits<double>::quiet_NaN()), 0.0),
                "NaN fixed dt yields 0")) {
        return 14;
    }

    // The helper agrees with the loop: after a half-step advance the accumulator reports 0.5, and a
    // render that coincides with a tick reports 0 (never a stale fraction of the previous step).
    FixedStepLoop halved(FixedStepConfig{60.0, 250.0, 5, 1.0});
    result = halved.advance(dt * 0.5);
    if (!expect(result.steps == 0, "half a step ticks nothing") ||
        !expect(near(result.alpha, 0.5), "half-step result alpha") ||
        !expect(near(interpolation_alpha(halved.accumulator(), halved.fixed_dt()), result.alpha),
                "free helper matches the loop accumulator") ||
        !expect(near(halved.alpha(), result.alpha), "loop accessor matches the result")) {
        return 15;
    }
    result = halved.advance(dt * 0.5);
    if (!expect(result.steps == 1, "the half steps complete one tick") ||
        !expect(near(result.alpha, 0.0), "alpha is 0 when a render coincides with the tick") ||
        !expect(near(interpolation_alpha(halved.accumulator(), halved.fixed_dt()), 0.0),
                "coincident tick stays at alpha 0")) {
        return 16;
    }

    std::cout << "fixed_step: ok\n";
    return 0;
}
