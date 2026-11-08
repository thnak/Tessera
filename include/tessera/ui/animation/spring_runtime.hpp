// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/animation/spring.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tessera::ui::animation {

struct SpringAnimationInput {
    std::uint64_t target = 0;
    std::string property;
    float from = 0.0f;
    float to = 0.0f;
    SpringConfig config;
    AnimationCallback on_complete;
};

class SpringRuntime {
public:
    AnimationId start(SpringAnimationInput input);
    bool retarget(AnimationId id, float target);

    [[nodiscard]] AnimationTickResult tick(double dt);
    bool stop(AnimationId id);
    void clear();

    [[nodiscard]] std::size_t active() const {
        return entries_.size();
    }
    [[nodiscard]] bool empty() const {
        return entries_.empty();
    }
    void set_reduced_motion(bool enabled) {
        reduced_motion_ = enabled;
    }
    [[nodiscard]] bool reduced_motion() const {
        return reduced_motion_;
    }

private:
    struct Entry {
        AnimationId id = 0;
        std::uint64_t target = 0;
        std::string property;
        Spring spring;
        AnimationCallback on_complete;
    };

    std::vector<Entry> entries_;
    AnimationId next_id_ = 1;
    bool reduced_motion_ = false;
};

} // namespace tessera::ui::animation
