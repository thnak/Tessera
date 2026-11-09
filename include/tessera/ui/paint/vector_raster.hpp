// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_sink.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace tessera::ui::paint {

// Resolved raster request for one VectorDraw: the destination content box plus the pixel extents a
// sink must rasterize the document at, and the uniform scale that maps document units to those
// pixels. GPU and CPU sinks both go through this helper so their size/scale rule cannot drift.
struct VectorRasterRequest {
    rhi::Rect dest;                 // absolute destination content box (device pixels)
    std::uint32_t raster_width = 0; // raster extent in device pixels
    std::uint32_t raster_height = 0;
    std::uint32_t scale_milli = 1000; // raster pixels per document logical unit (document width)
};

constexpr std::uint32_t kMaxVectorRasterExtent = 16384;
constexpr float kMinVectorScale = 0.01f;
constexpr float kMaxVectorScale = 64.0f;

// D10 size rule, in one place:
// - `scale` (or the sink device scale when `scale` is 0) is clamped to [0.01, 64];
// - both raster extents 0: content box size x scale;
// - one extent 0: derived from the other through the document aspect;
// - explicit extents win, clamped to [1, 16384];
// - `scale_milli` is uniform from the raster width over the document width, clamped to
//   [1, 16000] so an explicit size that ignores the document aspect stretches the texture.
// Returns nullopt for a degenerate rect/scale/document; callers treat that as a no-op.
[[nodiscard]] inline std::optional<VectorRasterRequest>
resolve_vector_raster(const VectorDraw& draw, std::uint32_t document_width,
                      std::uint32_t document_height, float device_scale) noexcept {
    if (!(draw.rect.width > 0.0f) || !(draw.rect.height > 0.0f) || document_width == 0U ||
        document_height == 0U) {
        return std::nullopt;
    }
    const float requested_scale = draw.scale > 0.0f ? draw.scale : device_scale;
    if (!(requested_scale > 0.0f) || !std::isfinite(requested_scale)) {
        return std::nullopt;
    }
    const float scale = std::clamp(requested_scale, kMinVectorScale, kMaxVectorScale);
    const auto clamp_extent = [](double value) {
        if (!(value > 0.0)) {
            return 1U;
        }
        return std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::llround(value)), 1U,
                                         kMaxVectorRasterExtent);
    };

    const double document_ratio =
        static_cast<double>(document_height) / static_cast<double>(document_width);
    std::uint32_t raster_width = draw.raster_width;
    std::uint32_t raster_height = draw.raster_height;
    if (raster_width == 0U && raster_height == 0U) {
        raster_width = clamp_extent(static_cast<double>(draw.rect.width) * scale);
        raster_height = clamp_extent(static_cast<double>(draw.rect.height) * scale);
    } else if (raster_width == 0U) {
        raster_width = clamp_extent(static_cast<double>(raster_height) / document_ratio);
    } else if (raster_height == 0U) {
        raster_height = clamp_extent(static_cast<double>(raster_width) * document_ratio);
    }
    raster_width = std::clamp<std::uint32_t>(raster_width, 1, kMaxVectorRasterExtent);
    raster_height = std::clamp<std::uint32_t>(raster_height, 1, kMaxVectorRasterExtent);

    const double milli =
        1000.0 * static_cast<double>(raster_width) / static_cast<double>(document_width);
    VectorRasterRequest request;
    request.dest = draw.rect;
    request.raster_width = raster_width;
    request.raster_height = raster_height;
    request.scale_milli =
        static_cast<std::uint32_t>(std::clamp<double>(std::llround(milli), 1.0, 16000.0));
    return request;
}

} // namespace tessera::ui::paint
