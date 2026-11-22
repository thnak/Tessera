// SPDX-License-Identifier: MIT
//
// Pure decode of Android window metrics (issue #189). No Android or JNI header is included, so this
// compiles and runs on every lane; the JNI bridge calls into it.

#include "android_metrics_translation.hpp"

#include <algorithm>
#include <cmath>
#include <span>

namespace tessera::hal::android {
namespace {

// Rounds a physical value to the nearest logical pixel. Truncating would lose a pixel per side on a
// 2.75x device, which shifts every row of the layout.
float round_logical(float value) {
    return std::round(value);
}

} // namespace

float logical_inset(std::int32_t physical, float scale) noexcept {
    if (physical <= 0) {
        return 0.0F;
    }
    const float divisor = scale > 0.0F ? scale : 1.0F;
    return round_logical(static_cast<float>(physical) / divisor);
}

WindowInsets system_bar_insets(const PhysicalInsets& physical, float scale) noexcept {
    WindowInsets insets;
    insets.left = logical_inset(physical.left, scale);
    insets.top = logical_inset(physical.top, scale);
    insets.right = logical_inset(physical.right, scale);
    insets.bottom = logical_inset(physical.bottom, scale);
    return insets;
}

WindowInsets ime_insets(const PhysicalInsets& physical, float scale) noexcept {
    // The IME is a bottom inset in practice, but the platform reports all four sides and a floating
    // keyboard can use any of them, so every side is carried rather than only the bottom.
    return system_bar_insets(physical, scale);
}

WindowInsets gesture_insets(const PhysicalInsets& physical, float scale) noexcept {
    return system_bar_insets(physical, scale);
}

CutoutInfo cutout_info(std::span<const PhysicalRect> rects, float scale) noexcept {
    CutoutInfo info;
    const float divisor = scale > 0.0F ? scale : 1.0F;
    for (const PhysicalRect& rect : rects) {
        if (info.count == kMaxCutouts) {
            break;
        }
        // The platform reports right/bottom as exclusive edges; the engine's rectangles are
        // x/y/width/height, so the extent is the difference.
        const std::int32_t width = rect.right - rect.left;
        const std::int32_t height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0) {
            // A zero-area cutout is not a cutout: reporting one would inset the layout by nothing
            // while claiming a notch exists.
            continue;
        }
        WindowRect out;
        out.x = round_logical(static_cast<float>(rect.left) / divisor);
        out.y = round_logical(static_cast<float>(rect.top) / divisor);
        out.width = round_logical(static_cast<float>(width) / divisor);
        out.height = round_logical(static_cast<float>(height) / divisor);
        info.bounds[info.count++] = out;
    }
    return info;
}

float hinge_angle(float reported_degrees) noexcept {
    // A negative reading is the platform's "no sensor" answer, not a measurement: pass it through
    // so a caller can tell "closed" (0) from "unknown" (-1). A non-finite reading is a broken HAL,
    // which is also unknown rather than a clamp to 0 that would look like a closed hinge.
    if (!std::isfinite(reported_degrees) || reported_degrees < 0.0F) {
        return kUnknownHingeAngle;
    }
    return std::min(reported_degrees, kMaxHingeAngleDegrees);
}

bool hinge_angle_is_known(float angle_degrees) noexcept {
    return std::isfinite(angle_degrees) && angle_degrees >= 0.0F;
}

FoldInfo fold_info(FoldPosture posture, FoldAxis axis, const PhysicalRect& bounds, float scale,
                   float angle_degrees) noexcept {
    FoldInfo info;
    // The angle is set before the posture switch because it is independent of it: a device can have
    // a hinge sensor and still report no `FoldingFeature` (the library answers nothing while the
    // window is not laid out), and dropping the angle in that case would make a foldable look like
    // a slab for as long as the posture is unknown.
    info.hinge_angle_degrees = hinge_angle(angle_degrees);
    switch (posture) {
    case FoldPosture::Flat:
        info.state = FoldState::Flat;
        break;
    case FoldPosture::HalfOpened:
        info.state = FoldState::HalfOpened;
        break;
    case FoldPosture::None:
        // No fold: the geometry keeps its "no fold" default, which is what a non-foldable reports.
        return info;
    }
    switch (axis) {
    case FoldAxis::Horizontal:
        info.orientation = FoldOrientation::Horizontal;
        break;
    case FoldAxis::Vertical:
        info.orientation = FoldOrientation::Vertical;
        break;
    case FoldAxis::None:
        info.orientation = FoldOrientation::None;
        break;
    }
    // Only a half-opened fold occludes: a flat fold is a seam, not a barrier, so its hinge is not
    // reported as an occluding rectangle.
    if (posture == FoldPosture::HalfOpened) {
        const float divisor = scale > 0.0F ? scale : 1.0F;
        const std::int32_t width = bounds.right - bounds.left;
        const std::int32_t height = bounds.bottom - bounds.top;
        if (width > 0 && height > 0) {
            info.occlusion = FoldOcclusion::Full;
            info.hinge_bounds.x = round_logical(static_cast<float>(bounds.left) / divisor);
            info.hinge_bounds.y = round_logical(static_cast<float>(bounds.top) / divisor);
            info.hinge_bounds.width = round_logical(static_cast<float>(width) / divisor);
            info.hinge_bounds.height = round_logical(static_cast<float>(height) / divisor);
        }
    }
    return info;
}

Orientation orientation_from_extent(std::uint32_t logical_width,
                                    std::uint32_t logical_height) noexcept {
    // Square is landscape, matching the rule the window backend already applies to its own metrics.
    return logical_width >= logical_height ? Orientation::Landscape : Orientation::Portrait;
}

} // namespace tessera::hal::android
