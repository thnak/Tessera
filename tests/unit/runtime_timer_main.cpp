// SPDX-License-Identifier: MIT
//
// The layer-0 timer wake source (AF2, issue #174; ADR-050 section 5 item 2). Two acceptance
// clauses, in the order the issue states them:
//
//   1. timers fire in order - earliest deadline first, and equal deadlines break by registration
//      order, deterministically (`runtime_timer_tests.cpp`, group `run_ordering_tests`);
//   2. an idle loop whose only pending work is a far timer parks: zero presents, zero extra ticks,
//      no polling (`runtime_timer_park_tests.cpp`, group `run_park_tests`).
//
// Deterministic by construction: every deadline is an absolute instant on a `ManualClock`, the
// queue is fired by hand, and nothing sleeps, spins or starts a thread.

#include "runtime_timer_test_support.hpp"

#include <iostream>

int main() {
    const int failures =
        runtime_timer_test::run_ordering_tests() + runtime_timer_test::run_park_tests();
    if (failures != 0) {
        std::cerr << "runtime_timer: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "runtime_timer: ok\n";
    return 0;
}
