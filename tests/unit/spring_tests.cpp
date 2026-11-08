// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/spring.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

using tessera::ui::animation::Spring;
using tessera::ui::animation::SpringConfig;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "spring: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs, float tolerance = 0.001f) {
    return std::abs(lhs - rhs) <= tolerance;
}

} // namespace

int main() {
    Spring exact(SpringConfig{100.0f, 0.0f, 1.0f, 0.01f, 0.01f});
    exact.set_target(10.0f);
    if (!expect(exact.tick(0.1f), "first step active") ||
        !expect(near(exact.position(), 10.0f), "first step position") ||
        !expect(near(exact.velocity(), 100.0f), "first step velocity")) {
        return 1;
    }
    if (!expect(exact.tick(0.1f), "second step active") ||
        !expect(near(exact.position(), 20.0f), "second step position")) {
        return 2;
    }

    Spring damped(SpringConfig{180.0f, 20.0f, 1.0f, 0.01f, 0.01f});
    damped.set_target(1.0f);
    int ticks = 0;
    while (damped.tick(0.016f) && ticks < 1000) {
        ++ticks;
    }
    if (!expect(!damped.active(), "damped settles") ||
        !expect(near(damped.position(), 1.0f), "damped position") ||
        !expect(near(damped.velocity(), 0.0f), "damped velocity") ||
        !expect(ticks < 1000, "damped convergence bound")) {
        return 3;
    }

    Spring retarget(SpringConfig{100.0f, 10.0f, 1.0f, 0.01f, 0.01f});
    retarget.set_target(1.0f);
    retarget.tick(0.05f);
    const float before_position = retarget.position();
    const float before_velocity = retarget.velocity();
    retarget.set_target(2.0f);
    if (!expect(near(retarget.position(), before_position), "retarget keeps position") ||
        !expect(near(retarget.velocity(), before_velocity), "retarget keeps velocity") ||
        !expect(near(retarget.target(), 2.0f), "retarget target") ||
        !expect(retarget.active(), "retarget active")) {
        return 4;
    }

    Spring snapped;
    snapped.snap_to(5.0f);
    if (!expect(!snapped.active(), "snap inactive") ||
        !expect(near(snapped.position(), 5.0f), "snap position") ||
        !expect(!snapped.tick(0.1f), "snap tick inactive")) {
        return 5;
    }

    Spring zero_dt(SpringConfig{100.0f, 0.0f, 1.0f, 0.01f, 0.01f});
    zero_dt.set_target(1.0f);
    if (!expect(zero_dt.tick(0.0f), "zero dt active") ||
        !expect(near(zero_dt.position(), 0.0f), "zero dt no movement") ||
        !expect(zero_dt.tick(std::nanf("")), "nan dt active") ||
        !expect(near(zero_dt.position(), 0.0f), "nan dt no movement") ||
        !expect(zero_dt.tick(-1.0f), "negative dt active") ||
        !expect(near(zero_dt.position(), 0.0f), "negative dt no movement")) {
        return 6;
    }

    bool threw = false;
    try {
        Spring invalid(SpringConfig{100.0f, 0.0f, 0.0f, 0.01f, 0.01f});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero mass rejected")) {
        return 7;
    }

    threw = false;
    try {
        Spring bad_target;
        bad_target.set_target(std::nanf(""));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "nan target rejected")) {
        return 8;
    }

    Spring deterministic_a(SpringConfig{180.0f, 20.0f, 1.0f, 0.01f, 0.01f});
    Spring deterministic_b(SpringConfig{180.0f, 20.0f, 1.0f, 0.01f, 0.01f});
    deterministic_a.set_target(3.0f);
    deterministic_b.set_target(3.0f);
    for (int i = 0; i < 100; ++i) {
        deterministic_a.tick(0.01f);
        deterministic_b.tick(0.01f);
        if (!expect(near(deterministic_a.position(), deterministic_b.position()) &&
                        near(deterministic_a.velocity(), deterministic_b.velocity()),
                    "deterministic integration")) {
            return 9;
        }
    }

    std::cout << "spring: ok\n";
    return 0;
}
