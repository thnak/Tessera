// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animated_scalar.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace {

using tessera::ui::animation::AnimatedScalar;
using tessera::ui::animation::Easing;
using tessera::ui::animation::ScalarOptions;
using tessera::ui::animation::SpringConfig;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animated_scalar: " << message << "\n";
        ++failures;
    }
    return condition;
}

bool near(double lhs, double rhs, double tolerance = 1e-6) {
    return std::abs(lhs - rhs) <= tolerance;
}

ScalarOptions spring_options(float stiffness, float damping, float mass = 1.0f) {
    ScalarOptions options;
    SpringConfig config;
    config.stiffness = stiffness;
    config.damping = damping;
    config.mass = mass;
    options.spring = config;
    return options;
}

// Ticks at 60 Hz until the scalar settles; returns the frames used (or `limit` if it never did).
int settle(AnimatedScalar& scalar, int limit = 2000) {
    int frames = 0;
    while (scalar.tick(1.0 / 60.0) && frames < limit) {
        ++frames;
    }
    return frames;
}

ScalarOptions linear(double duration_s) {
    ScalarOptions options;
    options.duration_s = duration_s;
    options.easing = Easing{};
    return options;
}

} // namespace

int main() {
    // The initial value is the value; nothing moves until a set.
    {
        AnimatedScalar scalar(1.0);
        expect(near(scalar.value(), 1.0) && near(scalar.target(), 1.0) && !scalar.active(),
               "starts at its initial value");
        expect(!scalar.tick(0.1) && near(scalar.value(), 1.0), "an idle scalar ignores ticks");
    }

    // A linear tween moves proportionally, reports activity, and lands exactly on the target.
    {
        AnimatedScalar scalar(0.0);
        scalar.set(10.0, linear(1.0));
        expect(scalar.active() && near(scalar.value(), 0.0), "set does not jump");
        expect(scalar.tick(0.25) && near(scalar.value(), 2.5), "quarter of the way");
        expect(scalar.tick(0.25) && near(scalar.value(), 5.0), "halfway");
        expect(!scalar.tick(0.75) && near(scalar.value(), 10.0) && !scalar.active(),
               "an overshooting tick lands exactly on the target");
        expect(near(scalar.target(), 10.0), "target is kept");
    }

    // The default easing is ease-out: it is ahead of linear early on.
    {
        AnimatedScalar scalar(0.0);
        scalar.set(1.0);
        scalar.tick(0.05);
        expect(scalar.value() > 0.25, "default ease-out is ahead of linear at 25%");
        scalar.tick(1.0);
        expect(near(scalar.value(), 1.0), "default tween finishes");
    }

    // Retargeting mid-flight starts from the current value, with no jump.
    {
        AnimatedScalar scalar(0.0);
        scalar.set(10.0, linear(1.0));
        scalar.tick(0.5);
        const double before = scalar.value();
        scalar.set(0.0, linear(1.0));
        expect(near(scalar.value(), before), "retarget keeps the current value");
        scalar.tick(0.5);
        expect(near(scalar.value(), before / 2.0), "retarget moves from the current value");
    }

    // animate:false snaps and cancels an in-flight tween; so do a zero duration and an equal
    // target.
    {
        AnimatedScalar scalar(0.0);
        scalar.set(10.0, linear(1.0));
        scalar.tick(0.1);
        ScalarOptions snap;
        snap.animate = false;
        scalar.set(3.0, snap);
        expect(near(scalar.value(), 3.0) && !scalar.active(), "animate:false snaps");
        scalar.set(4.0, linear(0.0));
        expect(near(scalar.value(), 4.0) && !scalar.active(), "zero duration snaps");
        scalar.set(4.0, linear(1.0));
        expect(!scalar.active(), "setting the current value starts nothing");
    }

    // Bad input is ignored: a non-finite target, a non-finite or negative tick.
    {
        AnimatedScalar scalar(2.0);
        scalar.set(std::numeric_limits<double>::quiet_NaN());
        scalar.set(std::numeric_limits<double>::infinity());
        expect(near(scalar.value(), 2.0) && !scalar.active(), "non-finite targets are ignored");
        scalar.set(6.0, linear(1.0));
        scalar.tick(std::numeric_limits<double>::quiet_NaN());
        scalar.tick(-1.0);
        expect(near(scalar.value(), 2.0) && scalar.active(), "bad ticks do not advance");
        AnimatedScalar nan_start(std::numeric_limits<double>::quiet_NaN());
        expect(near(nan_start.value(), 0.0), "a non-finite initial value becomes zero");
    }

    // Reduced motion snaps new sets and finishes an in-flight tween.
    {
        AnimatedScalar scalar(0.0);
        scalar.set(10.0, linear(1.0));
        scalar.tick(0.1);
        scalar.set_reduced_motion(true);
        expect(near(scalar.value(), 10.0) && !scalar.active(), "reduced motion finishes the tween");
        scalar.set(20.0, linear(1.0));
        expect(near(scalar.value(), 20.0) && !scalar.active(), "reduced motion snaps sets");
        scalar.set_reduced_motion(false);
        scalar.set(0.0, linear(1.0));
        expect(scalar.active(), "tweens resume once reduced motion is off");
    }

    // A spring moves by physics: no jump on set, an underdamped one overshoots, and it lands
    // exactly on the target when it settles.
    {
        AnimatedScalar scalar(0.0);
        scalar.set(10.0, spring_options(180.0f, 8.0f));
        expect(scalar.active() && near(scalar.value(), 0.0), "a spring set does not jump");
        double peak = 0.0;
        int frames = 0;
        while (scalar.tick(1.0 / 60.0) && frames++ < 2000) {
            peak = std::max(peak, scalar.value());
            expect(std::isfinite(scalar.value()), "the spring stays finite");
        }
        expect(frames < 2000, "an underdamped spring settles");
        expect(peak > 10.0, "an underdamped spring overshoots its target");
        expect(near(scalar.value(), 10.0) && !scalar.active(), "it lands exactly on the target");
    }

    // A critically damped spring does not overshoot.
    {
        AnimatedScalar scalar(0.0);
        scalar.set(10.0, spring_options(100.0f, 20.0f));
        double peak = 0.0;
        int frames = 0;
        while (scalar.tick(1.0 / 60.0) && frames++ < 2000) {
            peak = std::max(peak, scalar.value());
        }
        expect(peak <= 10.0 + 1e-3, "a critically damped spring does not overshoot");
        expect(near(scalar.value(), 10.0), "and still arrives");
    }

    // Retargeting a moving spring keeps its velocity: the value keeps going up for a moment even
    // though the new target is below it.
    {
        AnimatedScalar scalar(0.0);
        scalar.set(10.0, spring_options(180.0f, 12.0f));
        for (int i = 0; i < 6; ++i) {
            scalar.tick(1.0 / 60.0);
        }
        const double before = scalar.value();
        scalar.set(0.0, spring_options(180.0f, 12.0f));
        expect(near(scalar.value(), before), "a spring retarget does not jump");
        scalar.tick(1.0 / 120.0);
        expect(scalar.value() > before, "a spring retarget keeps its velocity");
        settle(scalar);
        expect(near(scalar.value(), 0.0) && !scalar.active(), "it then settles on the new target");
    }

    // A large travel settles (float positions cannot reach an absolute epsilon there), and a very
    // stiff light spring stays stable under a 60 Hz tick thanks to sub-stepping.
    {
        AnimatedScalar far(0.0);
        far.set(1.0e6, spring_options(180.0f, 20.0f));
        expect(settle(far, 4000) < 4000 && near(far.value(), 1.0e6, 1e-3),
               "a spring over a large distance settles");
        AnimatedScalar stiff(0.0);
        stiff.set(1.0, spring_options(100000.0f, 50.0f, 0.1f));
        bool stable = true;
        int frames = 0;
        while (stiff.tick(1.0 / 60.0) && frames++ < 4000) {
            stable = stable && std::isfinite(stiff.value()) && std::abs(stiff.value()) < 10.0;
        }
        expect(stable && near(stiff.value(), 1.0), "a very stiff spring stays stable");
        AnimatedScalar stalled(0.0);
        stalled.set(1.0, spring_options(180.0f, 20.0f));
        stalled.tick(60.0);
        expect(std::isfinite(stalled.value()) && std::abs(stalled.value()) < 10.0,
               "a stalled frame is bounded");
    }

    // Bad coefficients fall back to the defaults instead of throwing out of set().
    {
        AnimatedScalar scalar(0.0);
        scalar.set(1.0, spring_options(std::numeric_limits<float>::quiet_NaN(), -3.0f, 0.0f));
        expect(scalar.active(), "a spring with bad coefficients still runs");
        expect(settle(scalar) < 2000 && near(scalar.value(), 1.0), "using the defaults");
    }

    // A spring yields to a later tween from the current value, and to reduced motion.
    {
        AnimatedScalar scalar(0.0);
        scalar.set(10.0, spring_options(180.0f, 12.0f));
        for (int i = 0; i < 4; ++i) {
            scalar.tick(1.0 / 60.0);
        }
        const double mid = scalar.value();
        scalar.set(0.0, linear(1.0));
        expect(near(scalar.value(), mid), "a tween after a spring starts from the current value");
        scalar.tick(0.5);
        expect(near(scalar.value(), mid / 2.0), "and moves linearly");
        scalar.set(10.0, spring_options(180.0f, 12.0f));
        scalar.tick(1.0 / 60.0);
        scalar.set_reduced_motion(true);
        expect(near(scalar.value(), 10.0) && !scalar.active(), "reduced motion ends a spring");
        scalar.set(3.0, spring_options(180.0f, 12.0f));
        expect(near(scalar.value(), 3.0) && !scalar.active(), "and snaps a new spring set");
    }

    return failures == 0 ? 0 : 1;
}
