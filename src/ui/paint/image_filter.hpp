// SPDX-License-Identifier: MIT
//
// The image-sampling filter decision of display-aware rendering
// (docs/design/display-aware-rendering.md section 4, slice S8). One image can be cached with either
// filter, so this is a per-draw choice: a draw that lands exactly on the uploaded raster's pixels
// samples nearest (pixel-exact, the deterministic reference), and every other draw samples linear.
//
// The design's third case - mips when downscaling - is `image_levels_for_draw` below (RHI 1.6,
// issue #491): a draw that minifies the raster by at least 2x on either axis samples a mip chain
// built from the raster, when the device can build one.

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cmath>

namespace tessera::ui::paint {

// Same alias as paint_sink.hpp; repeated so this header is self-contained (a caller may include it
// before the paint headers, as paint_sinks.cpp's sorted include block does).
namespace rhi = tessera::rendering::rhi;

// A drawn device extent counts as the raster's own pixels when it is within half a device pixel on
// both axes: the raster extent is `round(content box x device scale)`, so rounding alone can never
// exceed this bound and a fractional device scale stays nearest when it still lands 1:1.
inline constexpr float kImageFilterExtentTolerance = 0.5f;

// A "whole pixel" origin is exact up to float noise from the CSS -> device multiplication (for
// example 0.8f * 1.25f == 1.0000000149f). A visible fraction such as 0.25 is far outside it.
inline constexpr float kImageFilterOriginTolerance = 1.0e-3f;

// A draw whose raster is at least this many times larger than the drawn extent on either axis gets
// a mip chain; below it level 0 filtered linearly is already within a texel of the right answer.
inline constexpr float kImageMipMinification = 2.0f;

// The filter one image draw samples with, decided from the draw's geometry in device space:
//   - `rect_device` is the destination rect mapped to device pixels (content box x device scale);
//   - `raster_extent` is the uploaded texture's extent (the cache key's width/height);
//   - `offset_x_device`/`offset_y_device` are the rect's device-space origin, tested separately
//     because a whole-pixel origin is its own condition.
// Nearest only when the drawn extent equals the raster extent within
// `kImageFilterExtentTolerance` on both axes AND the origin is a whole device pixel. Linear
// otherwise. A zero or non-finite request is degenerate (the paint pass would skip it): the
// deterministic nearest default is the safe answer, never a sampling mode that depends on junk.
[[nodiscard]] inline rhi::Filter image_filter_for_draw(const rhi::Rect& rect_device,
                                                       rhi::Extent2D raster_extent,
                                                       float offset_x_device,
                                                       float offset_y_device) noexcept {
    if (!(rect_device.width > 0.0f) || !(rect_device.height > 0.0f) || raster_extent.width == 0U ||
        raster_extent.height == 0U) {
        return rhi::Filter::Nearest;
    }
    if (!std::isfinite(rect_device.width) || !std::isfinite(rect_device.height) ||
        !std::isfinite(offset_x_device) || !std::isfinite(offset_y_device)) {
        return rhi::Filter::Nearest;
    }

    const auto extent_aligned = [](float drawn, std::uint32_t raster) {
        return std::fabs(drawn - static_cast<float>(raster)) <= kImageFilterExtentTolerance;
    };
    const auto whole_pixel = [](float value) {
        return std::fabs(value - std::round(value)) <= kImageFilterOriginTolerance;
    };

    const bool aligned = extent_aligned(rect_device.width, raster_extent.width) &&
                         extent_aligned(rect_device.height, raster_extent.height) &&
                         whole_pixel(offset_x_device) && whole_pixel(offset_y_device);
    return aligned ? rhi::Filter::Nearest : rhi::Filter::Linear;
}

// The mip-level count one image draw asks for: the raster's full chain when the device can build
// one (`mips_supported`) and the draw minifies the raster by at least `kImageMipMinification` on
// either axis, and 1 otherwise. A magnified or near-1:1 draw never pays the ~33% for a chain, and a
// degenerate or non-finite request is the single deterministic level.
[[nodiscard]] inline std::uint32_t image_levels_for_draw(const rhi::Rect& rect_device,
                                                         rhi::Extent2D raster_extent,
                                                         bool mips_supported) noexcept {
    if (!mips_supported || !(rect_device.width > 0.0f) || !(rect_device.height > 0.0f) ||
        !std::isfinite(rect_device.width) || !std::isfinite(rect_device.height) ||
        raster_extent.width == 0U || raster_extent.height == 0U) {
        return 1U;
    }
    const float shrink_x = static_cast<float>(raster_extent.width) / rect_device.width;
    const float shrink_y = static_cast<float>(raster_extent.height) / rect_device.height;
    if (shrink_x < kImageMipMinification && shrink_y < kImageMipMinification) {
        return 1U;
    }
    return rhi::full_mip_levels(raster_extent);
}

} // namespace tessera::ui::paint
