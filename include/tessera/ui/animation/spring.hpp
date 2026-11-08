// SPDX-License-Identifier: MIT

#pragma once

namespace tessera::ui::animation {

struct SpringConfig {
    float stiffness = 180.0f;
    float damping = 20.0f;
    float mass = 1.0f;
    float position_epsilon = 0.01f;
    float velocity_epsilon = 0.01f;
};

class Spring {
public:
    explicit Spring(SpringConfig config = {});

    void set_target(float target);
    void snap_to(float position);

    bool tick(float dt);

    [[nodiscard]] float position() const {
        return position_;
    }
    [[nodiscard]] float velocity() const {
        return velocity_;
    }
    [[nodiscard]] float target() const {
        return target_;
    }
    [[nodiscard]] bool active() const {
        return active_;
    }
    [[nodiscard]] const SpringConfig& config() const {
        return config_;
    }

private:
    SpringConfig config_;
    float position_ = 0.0f;
    float velocity_ = 0.0f;
    float target_ = 0.0f;
    bool active_ = false;
};

} // namespace tessera::ui::animation
