// SPDX-License-Identifier: MIT
//
// Shared fixture and assertions for the layer-0 timer wake-source tests (AF2, issue #174). Both
// halves are compiled into one `runtime_timer` binary: `runtime_timer_tests.cpp` covers ordering
// and handle safety, `runtime_timer_park_tests.cpp` covers the parking acceptance clause.

#pragma once

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/runtime/timer_queue.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace runtime_timer_test {

namespace runtime = tessera::runtime;

using runtime::FixedStepConfig;
using runtime::kNoDeadline;
using runtime::ManualClock;
using runtime::RuntimeLoop;
using runtime::RuntimeLoopConfig;
using runtime::RuntimeTick;
using runtime::TimerId;
using runtime::TimerQueue;

inline int failures = 0;

inline bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "runtime_timer: " << message << "\n";
        ++failures;
    }
    return condition;
}

inline bool near(double value, double expected) {
    return std::fabs(value - expected) <= 1.0e-9;
}

// True when `call` throws `std::invalid_argument`. A small helper keeps the error-contract case
// readable without four copies of the same try/catch.
template<typename Call> bool throws_invalid_argument(Call call) {
    try {
        call();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

// A loop configured the way a host configures it: on-demand rendering (so nothing presents without
// a request) with the timer queue attached.
inline RuntimeLoop make_loop(ManualClock& clock, TimerQueue& timers) {
    RuntimeLoopConfig config;
    config.fixed_step = FixedStepConfig{60.0, 250.0, 5, 1.0};
    config.render_on_demand = true;
    config.timers = &timers;
    return RuntimeLoop(clock, config);
}

// Drives `loop` the way the window driver does: tick, and "present" only when the loop says a frame
// is due. Nothing else marks the scene dirty, so a should_render here is a real render cause.
inline RuntimeTick step(RuntimeLoop& loop, int& presents) {
    const RuntimeTick tick = loop.tick();
    if (tick.should_render) {
        ++presents;
    }
    return tick;
}

int run_ordering_tests();
int run_park_tests();

} // namespace runtime_timer_test
