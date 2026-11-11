// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/image/image_resize.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace tessera::ui::paint {

// Resolved raster request for one content draw (vector asset or raster image): the destination
// content box plus the pixel extents a sink must rasterize/upload at, and the uniform scale that
// maps intrinsic content units to those pixels. GPU and CPU sinks both go through this helper so
// the size/scale rule cannot drift between content kinds.
struct ContentRasterRequest {
    rhi::Rect dest;                 // absolute destination content box (device pixels)
    std::uint32_t raster_width = 0; // raster extent in device pixels
    std::uint32_t raster_height = 0;
    std::uint32_t scale_milli = 1000; // raster pixels per intrinsic content unit (content width)
};

using VectorRasterRequest = ContentRasterRequest;

constexpr std::uint32_t kMaxContentRasterExtent = 16384;
constexpr float kMinContentScale = 0.01f;
constexpr float kMaxContentScale = 64.0f;

// D10/D12 size rule, in one place:
// - `scale` (or the sink device scale when `scale` is 0) is clamped to [0.01, 64];
// - both raster extents 0: content box size x scale;
// - one extent 0: derived from the other through the intrinsic content aspect;
// - explicit extents win, clamped to [1, 16384];
// - `scale_milli` is uniform from the raster width over the intrinsic width, clamped to
//   [1, 16000] so an explicit size that ignores the content aspect stretches the texture.
// Returns nullopt for a degenerate rect/scale/content; callers treat that as a no-op.
[[nodiscard]] inline std::optional<ContentRasterRequest>
resolve_content_raster(const rhi::Rect& dest, std::uint32_t raster_width,
                       std::uint32_t raster_height, float scale, std::uint32_t intrinsic_width,
                       std::uint32_t intrinsic_height, float device_scale) noexcept {
    if (!(dest.width > 0.0f) || !(dest.height > 0.0f) || intrinsic_width == 0U ||
        intrinsic_height == 0U) {
        return std::nullopt;
    }
    const float requested_scale = scale > 0.0f ? scale : device_scale;
    if (!(requested_scale > 0.0f) || !std::isfinite(requested_scale)) {
        return std::nullopt;
    }
    const float clamped_scale = std::clamp(requested_scale, kMinContentScale, kMaxContentScale);
    const auto clamp_extent = [](double value) {
        if (!(value > 0.0)) {
            return 1U;
        }
        return std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::llround(value)), 1U,
                                         kMaxContentRasterExtent);
    };

    const double content_ratio =
        static_cast<double>(intrinsic_height) / static_cast<double>(intrinsic_width);
    std::uint32_t width = raster_width;
    std::uint32_t height = raster_height;
    if (width == 0U && height == 0U) {
        width = clamp_extent(static_cast<double>(dest.width) * clamped_scale);
        height = clamp_extent(static_cast<double>(dest.height) * clamped_scale);
    } else if (width == 0U) {
        width = clamp_extent(static_cast<double>(height) / content_ratio);
    } else if (height == 0U) {
        height = clamp_extent(static_cast<double>(width) * content_ratio);
    }
    width = std::clamp<std::uint32_t>(width, 1, kMaxContentRasterExtent);
    height = std::clamp<std::uint32_t>(height, 1, kMaxContentRasterExtent);

    const double milli = 1000.0 * static_cast<double>(width) / static_cast<double>(intrinsic_width);
    ContentRasterRequest request;
    request.dest = dest;
    request.raster_width = width;
    request.raster_height = height;
    request.scale_milli = static_cast<std::uint32_t>(
        std::clamp<double>(static_cast<double>(std::llround(milli)), 1.0, 16000.0));
    return request;
}

[[nodiscard]] inline std::optional<ContentRasterRequest>
resolve_vector_raster(const VectorDraw& draw, std::uint32_t document_width,
                      std::uint32_t document_height, float device_scale) noexcept {
    return resolve_content_raster(draw.rect, draw.raster_width, draw.raster_height, draw.scale,
                                  document_width, document_height, device_scale);
}

[[nodiscard]] inline std::optional<ContentRasterRequest>
resolve_image_raster(const ImageDraw& draw, std::uint32_t image_width, std::uint32_t image_height,
                     float device_scale) noexcept {
    return resolve_content_raster(draw.rect, draw.raster_width, draw.raster_height, draw.scale,
                                  image_width, image_height, device_scale);
}

// The decode target of an image draw (issue #14): the raster extent the draw will upload (explicit
// extents, else content box x scale), bucketed. With one explicit extent the other is unknown, so
// the target box is square on the larger side and the decode stays at least as big as needed.
// `decode_original` yields the original (source-size) target.
[[nodiscard]] inline rendering::image::DecodeTarget
image_decode_target(const ImageDraw& draw, float device_scale) noexcept {
    if (draw.decode_original) {
        return {};
    }
    const float pixel_scale = draw.scale > 0.0f ? draw.scale : device_scale;
    float width = draw.raster_width != 0U ? static_cast<float>(draw.raster_width)
                                          : draw.rect.width * pixel_scale;
    float height = draw.raster_height != 0U ? static_cast<float>(draw.raster_height)
                                            : draw.rect.height * pixel_scale;
    if ((draw.raster_width == 0U) != (draw.raster_height == 0U)) {
        width = height = std::max(width, height);
    }
    return rendering::image::decode_target_for(width, height, 1.0f);
}

} // namespace tessera::ui::paint
