// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/spring.hpp"

#include <cmath>
#include <stdexcept>

namespace tessera::ui::animation {

namespace {

void validate(const SpringConfig& config) {
    if (!std::isfinite(config.stiffness) || config.stiffness < 0.0f ||
        !std::isfinite(config.damping) || config.damping < 0.0f || !std::isfinite(config.mass) ||
        config.mass <= 0.0f || !std::isfinite(config.position_epsilon) ||
        config.position_epsilon < 0.0f || !std::isfinite(config.velocity_epsilon) ||
        config.velocity_epsilon < 0.0f) {
        throw std::invalid_argument("invalid spring config");
    }
}

} // namespace

Spring::Spring(SpringConfig config)
    : config_(config) {
    validate(config_);
}

void Spring::set_config(SpringConfig config) {
    validate(config);
    config_ = config;
}

void Spring::set_target(float target) {
    if (!std::isfinite(target)) {
        throw std::invalid_argument("spring target must be finite");
    }
    target_ = target;
    active_ = true;
}

void Spring::snap_to(float position) {
    if (!std::isfinite(position)) {
        throw std::invalid_argument("spring position must be finite");
    }
    position_ = position;
    velocity_ = 0.0f;
    active_ = false;
}

bool Spring::tick(float dt) {
    if (!active_) {
        return false;
    }
    if (!std::isfinite(dt) || dt <= 0.0f) {
        return true;
    }

    const float displacement = position_ - target_;
    if (std::abs(displacement) <= config_.position_epsilon &&
        std::abs(velocity_) <= config_.velocity_epsilon) {
        position_ = target_;
        velocity_ = 0.0f;
        active_ = false;
        return false;
    }

    const float acceleration =
        (-config_.stiffness * displacement - config_.damping * velocity_) / config_.mass;
    velocity_ += acceleration * dt;
    position_ += velocity_ * dt;

    if (std::abs(position_ - target_) <= config_.position_epsilon &&
        std::abs(velocity_) <= config_.velocity_epsilon) {
        position_ = target_;
        velocity_ = 0.0f;
        active_ = false;
    }
    return active_;
}

} // namespace tessera::ui::animation
