// SPDX-License-Identifier: MIT

#include "tessera/runtime/clock.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "clock: " << message << "\n";
    }
    return condition;
}

bool near(double value, double expected, double tolerance = 1.0e-12) {
    return std::abs(value - expected) <= tolerance;
}

} // namespace

int main() {
    using tessera::runtime::ManualClock;

    ManualClock clock;
    if (!expect(near(clock.now_seconds(), 0.0), "default start")) {
        return 1;
    }

    clock.advance(0.5);
    if (!expect(near(clock.now_seconds(), 0.5), "advance")) {
        return 2;
    }

    clock.advance(0.25);
    if (!expect(near(clock.now_seconds(), 0.75), "second advance")) {
        return 3;
    }

    ManualClock started(10.0);
    if (!expect(near(started.now_seconds(), 10.0), "explicit start")) {
        return 4;
    }

    bool threw = false;
    try {
        clock.advance(-1.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "negative advance rejected")) {
        return 5;
    }

    threw = false;
    try {
        clock.advance(std::numeric_limits<double>::quiet_NaN());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "NaN advance rejected")) {
        return 6;
    }

    threw = false;
    try {
        clock.advance(std::numeric_limits<double>::infinity());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "infinite advance rejected")) {
        return 7;
    }

    threw = false;
    try {
        ManualClock invalid(-1.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "negative start rejected")) {
        return 8;
    }

    std::cout << "clock: ok\n";
    return 0;
}
