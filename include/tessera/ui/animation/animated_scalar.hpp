// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/easing.hpp"

namespace tessera::ui::animation {

// Options of one `AnimatedScalar::set`. The defaults are CSS `ease-out` over 200 ms, the same
// feel as a transition with no explicit timing; `animate=false` is `set(v, {animate: false})`.
struct ScalarOptions {
    double duration_s = 0.2;
    Easing easing = Easing{EasingKind::CubicBezier, 0.0f, 0.0f, 0.58f, 1.0f};
    bool animate = true;
};

// The scalar behind `useAnimatedValue` (docs/design/animation.md section 6): a number a component
// reads and sets, which moves toward its target on the runtime's frame ticks instead of jumping.
// It owns no node, host or thread and never reads a clock: the owner calls `tick(dt)` with the
// frame delta, so the value is deterministic and testable. Every set restarts the tween from the
// current value, so retargeting mid-flight never jumps.
class AnimatedScalar {
public:
    explicit AnimatedScalar(double initial = 0.0) noexcept;

    // Moves toward `target`. A non-finite target is ignored. Snaps when `animate` is false, the
    // duration is not positive, or reduced motion is on.
    void set(double target, const ScalarOptions& options = {}) noexcept;

    // Advances by `dt` seconds (non-finite or negative is ignored). Returns true while the value
    // is still moving afterwards, so the owner knows to request another frame; a tick that lands
    // on the target returns false.
    bool tick(double dt) noexcept;

    [[nodiscard]] double value() const noexcept {
        return value_;
    }
    [[nodiscard]] double target() const noexcept {
        return target_;
    }
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

    // Reduced motion snaps every set and finishes an in-flight tween at its target.
    void set_reduced_motion(bool enabled) noexcept;

private:
    double value_ = 0.0;
    double target_ = 0.0;
    double from_ = 0.0;
    double elapsed_s_ = 0.0;
    double duration_s_ = 0.0;
    Easing easing_{};
    bool active_ = false;
    bool reduced_motion_ = false;
};

} // namespace tessera::ui::animation
