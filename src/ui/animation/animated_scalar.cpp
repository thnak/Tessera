// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animated_scalar.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui::animation {
namespace {

// Longest frame delta one tick integrates: a stalled frame must not run the spring for seconds.
constexpr double kMaxSpringStepSeconds = 0.25;
// The integrator is semi-implicit Euler, stable while (angular frequency * step) stays below 2.
// The step is also capped at 1/240 s so a soft spring is sampled smoothly.
constexpr double kMaxSubstepSeconds = 1.0 / 240.0;
constexpr double kStabilityFactor = 0.5;
constexpr int kMaxSubsteps = 512;

// A spring comes from untrusted generated input; fall back to the default for any bad coefficient
// instead of letting `Spring` throw out of a noexcept setter.
SpringConfig sanitized(SpringConfig config) {
    const SpringConfig defaults;
    if (!std::isfinite(config.stiffness) || config.stiffness < 0.0f) {
        config.stiffness = defaults.stiffness;
    }
    if (!std::isfinite(config.damping) || config.damping < 0.0f) {
        config.damping = defaults.damping;
    }
    if (!std::isfinite(config.mass) || config.mass <= 0.0f) {
        config.mass = defaults.mass;
    }
    if (!std::isfinite(config.position_epsilon) || config.position_epsilon < 0.0f) {
        config.position_epsilon = defaults.position_epsilon;
    }
    if (!std::isfinite(config.velocity_epsilon) || config.velocity_epsilon < 0.0f) {
        config.velocity_epsilon = defaults.velocity_epsilon;
    }
    return config;
}

} // namespace

AnimatedScalar::AnimatedScalar(double initial) noexcept
    : value_(std::isfinite(initial) ? initial : 0.0),
      target_(value_),
      from_(value_) {}

void AnimatedScalar::set(double target, const ScalarOptions& options) noexcept {
    if (!std::isfinite(target)) {
        return;
    }
    const bool snap = !options.animate || reduced_motion_ || (target == value_ && !active_) ||
                      (!options.spring.has_value() &&
                       (!std::isfinite(options.duration_s) || options.duration_s <= 0.0));
    target_ = target;
    if (snap) {
        value_ = target;
        from_ = target;
        active_ = false;
        spring_mode_ = false;
        return;
    }
    if (options.spring.has_value()) {
        const SpringConfig config = sanitized(*options.spring);
        if (active_ && spring_mode_) {
            spring_.set_config(config); // keep position and velocity: a retarget never jumps
        } else {
            spring_ = Spring(config);
            spring_.snap_to(static_cast<float>(value_));
        }
        spring_.set_target(static_cast<float>(target));
        spring_mode_ = true;
        active_ = true;
        return;
    }
    spring_mode_ = false;
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
    if (spring_mode_) {
        const SpringConfig& config = spring_.config();
        const double omega =
            std::sqrt(static_cast<double>(config.stiffness) / static_cast<double>(config.mass));
        const double step_limit = omega > 0.0
                                      ? std::min(kMaxSubstepSeconds, kStabilityFactor / omega)
                                      : kMaxSubstepSeconds;
        const double span = std::min(dt, kMaxSpringStepSeconds);
        const int steps =
            std::clamp(static_cast<int>(std::ceil(span / step_limit)), 1, kMaxSubsteps);
        const auto step = static_cast<float>(span / steps);
        bool moving = true;
        for (int i = 0; i < steps && moving; ++i) {
            moving = spring_.tick(step);
        }
        // Float positions cannot reach an absolute epsilon at large magnitudes, so settle on a
        // relative bound as well; otherwise a spring toward 1e6 would tick forever.
        const double relative = std::abs(target_) * 1e-5;
        const double slack = std::max(static_cast<double>(config.position_epsilon), relative);
        const double speed_slack = std::max(static_cast<double>(config.velocity_epsilon), relative);
        if (moving && std::abs(spring_.position() - target_) <= slack &&
            std::abs(static_cast<double>(spring_.velocity())) <= speed_slack) {
            moving = false;
        }
        if (!moving) {
            value_ = target_;
            active_ = false;
            return false;
        }
        value_ = static_cast<double>(spring_.position());
        return true;
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
        spring_mode_ = false;
    }
}

} // namespace tessera::ui::animation
