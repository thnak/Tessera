// SPDX-License-Identifier: MIT

#pragma once

// Capsule morphing (docs/specs/live-capsule-v1.md section 3, issue #224): width, height and corner
// radius move together on the spring runtime. With reduced motion the size snaps and the state
// cross-fades instead. The morph wakes the pacer only while `active()`.

#include "tessera/ui/animation/spring.hpp"
#include "tessera/ui/capsule/capsule_state.hpp"

namespace tessera::ui::capsule {

class CapsuleMorph {
public:
    static constexpr float kCrossFadeSeconds = 0.15f;

    explicit CapsuleMorph(animation::SpringConfig config = {});

    // Sets the size at once (the first frame of an activity), with no animation.
    void snap(const CapsuleSize& size);
    // Retargets the spring (or snaps and starts the cross-fade under reduced motion).
    void set_target(const CapsuleSize& size);
    void set_reduced_motion(bool enabled);

    // Advances by `dt` seconds; true while anything still moves.
    bool tick(float dt);

    [[nodiscard]] CapsuleSize current() const noexcept;
    // 0 right after a reduced-motion state change, 1 once the cross-fade is done. Always 1 when
    // motion is not reduced.
    [[nodiscard]] float cross_fade() const noexcept {
        return cross_fade_;
    }
    [[nodiscard]] bool active() const noexcept;

private:
    animation::Spring width_;
    animation::Spring height_;
    animation::Spring radius_;
    bool reduced_motion_ = false;
    float cross_fade_ = 1.0f;
};

} // namespace tessera::ui::capsule
