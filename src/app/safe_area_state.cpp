// SPDX-License-Identifier: MIT

// The node half of SafeArea (the `tsafe` token parsing, `SafeAreaSpec`, and the padding/margin
// writer) lives in `src/ui/style/safe_area_style.cpp` since issue #408: the design-time preview is
// a second layout consumer and must resolve a `SafeArea` through the SAME code the app does, or a
// foldable would preview differently from the device. What stays here is only the app's part - the
// per-edge easing of an inset change - because a preview has no clock and applies a posture at
// once.

#include "safe_area_state.hpp"

#include <cmath>

namespace tessera::app {

namespace {

constexpr float kSnapDistance = 0.05f;

bool ease(float& value, float target, double factor) noexcept {
    if (value == target) {
        return false;
    }
    const float delta = target - value;
    if (std::fabs(delta) <= kSnapDistance) {
        value = target;
        return true;
    }
    value += delta * static_cast<float>(factor);
    return true;
}

bool ease(hal::WindowInsets& value, const hal::WindowInsets& target, double factor) noexcept {
    bool changed = false;
    changed |= ease(value.left, target.left, factor);
    changed |= ease(value.top, target.top, factor);
    changed |= ease(value.right, target.right, factor);
    changed |= ease(value.bottom, target.bottom, factor);
    return changed;
}

bool settled(const hal::WindowInsets& a, const hal::WindowInsets& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool same_rect(const hal::WindowRect& a, const hal::WindowRect& b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// True when every input of `resolve_safe_area` is equal.
bool same_inputs(const hal::WindowMetrics& a, const hal::WindowMetrics& b) noexcept {
    if (a.logical_width != b.logical_width || a.logical_height != b.logical_height ||
        !settled(a.insets, b.insets) || !settled(a.ime_insets, b.ime_insets) ||
        !settled(a.gesture_insets, b.gesture_insets) ||
        !settled(a.cutouts.waterfall_insets, b.cutouts.waterfall_insets) ||
        a.cutouts.count != b.cutouts.count || a.fold.occlusion != b.fold.occlusion ||
        a.fold.orientation != b.fold.orientation ||
        !same_rect(a.fold.hinge_bounds, b.fold.hinge_bounds)) {
        return false;
    }
    for (std::size_t i = 0; i < a.cutouts.count && i < hal::kMaxCutouts; ++i) {
        if (!same_rect(a.cutouts.bounds[i], b.cutouts.bounds[i])) {
            return false;
        }
    }
    return true;
}

bool animated_settled(const hal::WindowMetrics& a, const hal::WindowMetrics& b) noexcept {
    return settled(a.insets, b.insets) && settled(a.ime_insets, b.ime_insets) &&
           settled(a.gesture_insets, b.gesture_insets);
}

} // namespace

bool SafeAreaState::set_metrics(const hal::WindowMetrics& metrics, bool reduced_motion) {
    if (has_metrics_ && same_inputs(target_, metrics)) {
        return false;
    }
    target_ = metrics;
    if (!has_metrics_ || reduced_motion) {
        current_ = metrics;
        has_metrics_ = true;
        animating_ = false;
        return true;
    }
    // Everything but the three animated inset sets follows the platform at once.
    const hal::WindowInsets bars = current_.insets;
    const hal::WindowInsets ime = current_.ime_insets;
    const hal::WindowInsets gestures = current_.gesture_insets;
    current_ = metrics;
    current_.insets = bars;
    current_.ime_insets = ime;
    current_.gesture_insets = gestures;
    animating_ = !animated_settled(current_, target_);
    return true;
}

bool SafeAreaState::step(double dt, bool reduced_motion) {
    if (!animating_) {
        return false;
    }
    if (reduced_motion || !std::isfinite(dt) || dt < 0.0) {
        const bool changed = !animated_settled(current_, target_);
        current_.insets = target_.insets;
        current_.ime_insets = target_.ime_insets;
        current_.gesture_insets = target_.gesture_insets;
        animating_ = false;
        return changed;
    }
    const double factor = 1.0 - std::exp(-dt / kTimeConstantSeconds);
    bool changed = false;
    changed |= ease(current_.insets, target_.insets, factor);
    changed |= ease(current_.ime_insets, target_.ime_insets, factor);
    changed |= ease(current_.gesture_insets, target_.gesture_insets, factor);
    animating_ = !animated_settled(current_, target_);
    return changed;
}

} // namespace tessera::app
