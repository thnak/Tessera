// SPDX-License-Identifier: MIT

#include "tessera/ui/capsule/capsule_morph.hpp"

#include <algorithm>

namespace tessera::ui::capsule {

CapsuleMorph::CapsuleMorph(animation::SpringConfig config)
    : width_(config),
      height_(config),
      radius_(config) {}

void CapsuleMorph::snap(const CapsuleSize& size) {
    // snap_to leaves the target alone, so set it first: a later "settle at the target" must agree.
    width_.set_target(size.width);
    height_.set_target(size.height);
    radius_.set_target(size.corner_radius);
    width_.snap_to(size.width);
    height_.snap_to(size.height);
    radius_.snap_to(size.corner_radius);
    cross_fade_ = 1.0f;
}

void CapsuleMorph::set_target(const CapsuleSize& size) {
    if (reduced_motion_) {
        const CapsuleSize now = current();
        const bool changed = now.width != size.width || now.height != size.height ||
                             now.corner_radius != size.corner_radius;
        snap(size);
        if (changed) {
            cross_fade_ = 0.0f;
        }
        return;
    }
    width_.set_target(size.width);
    height_.set_target(size.height);
    radius_.set_target(size.corner_radius);
}

void CapsuleMorph::set_reduced_motion(bool enabled) {
    const bool turned_on = enabled && !reduced_motion_;
    reduced_motion_ = enabled;
    if (turned_on) {
        // Settle what is running at its target so no size animation survives the switch.
        width_.snap_to(width_.target());
        height_.snap_to(height_.target());
        radius_.snap_to(radius_.target());
    }
}

bool CapsuleMorph::tick(float dt) {
    bool moving = false;
    moving |= width_.tick(dt);
    moving |= height_.tick(dt);
    moving |= radius_.tick(dt);
    if (cross_fade_ < 1.0f) {
        cross_fade_ = std::min(1.0f, cross_fade_ + dt / kCrossFadeSeconds);
        moving |= cross_fade_ < 1.0f;
    }
    return moving;
}

CapsuleSize CapsuleMorph::current() const noexcept {
    return {width_.position(), height_.position(), radius_.position()};
}

bool CapsuleMorph::active() const noexcept {
    return width_.active() || height_.active() || radius_.active() || cross_fade_ < 1.0f;
}

} // namespace tessera::ui::capsule
