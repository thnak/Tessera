// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animated_scalar.hpp"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

using tessera::ui::animation::AnimatedScalar;
using tessera::ui::animation::Easing;
using tessera::ui::animation::ScalarOptions;

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

    return failures == 0 ? 0 : 1;
}
