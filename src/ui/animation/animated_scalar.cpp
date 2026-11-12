// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animated_scalar.hpp"

#include <cmath>

namespace tessera::ui::animation {

AnimatedScalar::AnimatedScalar(double initial) noexcept
    : value_(std::isfinite(initial) ? initial : 0.0),
      target_(value_),
      from_(value_) {}

void AnimatedScalar::set(double target, const ScalarOptions& options) noexcept {
    if (!std::isfinite(target)) {
        return;
    }
    const bool snap = !options.animate || reduced_motion_ || !std::isfinite(options.duration_s) ||
                      options.duration_s <= 0.0 || target == value_;
    target_ = target;
    if (snap) {
        value_ = target;
        from_ = target;
        active_ = false;
        return;
    }
    from_ = value_;
    elapsed_s_ = 0.0;
    duration_s_ = options.duration_s;
    easing_ = options.easing;
    active_ = true;
}

bool AnimatedScalar::tick(double dt) noexcept {
    if (!active_ || !std::isfinite(dt) || dt < 0.0) {
        return active_;
    }
    elapsed_s_ += dt;
    if (elapsed_s_ >= duration_s_) {
        value_ = target_;
        active_ = false;
        return false;
    }
    const auto progress = static_cast<float>(elapsed_s_ / duration_s_);
    value_ = from_ + ((target_ - from_) * static_cast<double>(ease(easing_, progress)));
    return true;
}

void AnimatedScalar::set_reduced_motion(bool enabled) noexcept {
    reduced_motion_ = enabled;
    if (enabled && active_) {
        value_ = target_;
        active_ = false;
    }
}

} // namespace tessera::ui::animation
