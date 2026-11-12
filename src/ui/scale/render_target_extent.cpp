// SPDX-License-Identifier: MIT

#include "tessera/ui/scale/render_scale.hpp"
#include "tessera/ui/scale/render_target_extent.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui::scale {
namespace {

std::uint32_t clamp_extent(float value) noexcept {
    const float clamped = std::clamp(value, 1.0f, static_cast<float>(kMaxRenderTargetExtent));
    return static_cast<std::uint32_t>(clamped);
}

// A decision the controller never produces (non-finite, zero, negative) is not a downgrade: it
// keeps the full-quality target instead of collapsing the scene to a 1x1 render.
float checked_adaptive_scale(float adaptive_scale) noexcept {
    if (!std::isfinite(adaptive_scale) || adaptive_scale <= 0.0f) {
        return 1.0f;
    }
    return adaptive_scale;
}

} // namespace

std::uint32_t presented_extent_pixels(float logical, float device_scale) noexcept {
    if (!std::isfinite(logical) || !std::isfinite(device_scale) || logical <= 0.0f ||
        device_scale <= 0.0f) {
        return 1;
    }
    return clamp_extent(std::ceil(logical * device_scale));
}

float glyph_raster_scale(float reported_device_scale) noexcept {
    // Deliberately the device ladder only. The adaptive multiplier is not a parameter, so no call
    // site can accidentally key the atlas by the quality decision.
    return quantize_render_scale(reported_device_scale);
}

std::uint32_t adaptive_extent_pixels(std::uint32_t presented, float adaptive_scale) noexcept {
    if (presented == 0) {
        return 1;
    }
    const float scale = checked_adaptive_scale(adaptive_scale);
    // floor, not round: a fractional extent may not claim a pixel of content that was not rendered
    // at that scale, and floor(1.0 * presented) is exactly `presented` for every presented extent
    // below the float-integer ceiling (the clamp keeps it there).
    //
    // The product is evaluated in float, like the controller's ladder value: a decimal multiplier
    // such as 0.9 is not representable exactly, and float rounding of 320 * 0.9f gives the 288 the
    // caller meant, where a wider evaluation would floor 287.999... to 287 and lose a pixel for a
    // representation artifact. Extents stay far below 2^24, so the precision is exact where it
    // matters.
    const float scaled = std::floor(static_cast<float>(presented) * scale);
    if (!(scaled >= 1.0f)) {
        return 1;
    }
    if (scaled >= static_cast<float>(kMaxRenderTargetExtent)) {
        return kMaxRenderTargetExtent;
    }
    return static_cast<std::uint32_t>(scaled);
}

AdaptiveTargetExtent adaptive_target_extent(rendering::rhi::Extent2D presented,
                                            float adaptive_scale) noexcept {
    AdaptiveTargetExtent result;
    result.presented =
        rendering::rhi::Extent2D{std::max(presented.width, 1u), std::max(presented.height, 1u)};
    result.extent =
        rendering::rhi::Extent2D{adaptive_extent_pixels(result.presented.width, adaptive_scale),
                                 adaptive_extent_pixels(result.presented.height, adaptive_scale)};
    result.scaled = result.extent.width != result.presented.width ||
                    result.extent.height != result.presented.height;
    if (result.scaled) {
        result.content_scale_x =
            static_cast<float>(result.extent.width) / static_cast<float>(result.presented.width);
        result.content_scale_y =
            static_cast<float>(result.extent.height) / static_cast<float>(result.presented.height);
    }
    return result;
}

} // namespace tessera::ui::scale
