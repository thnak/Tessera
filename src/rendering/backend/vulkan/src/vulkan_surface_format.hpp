// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace tessera::rendering::vulkan {

// Plain-data mirror of the Vulkan surface format/color-space values the swapchain selection policy
// cares about, so the policy is unit-testable without Vulkan headers or a GPU (the same approach as
// vulkan_device_selection.hpp). `Other` keeps the driver's original value on the caller side.
enum class SurfaceFormat : std::uint8_t {
    Undefined,
    R8G8B8A8Unorm,
    B8G8R8A8Unorm,
    R8G8B8A8Srgb,
    B8G8R8A8Srgb,
    Other,
};

enum class SurfaceColorSpace : std::uint8_t { SrgbNonlinear, Other };

struct SurfaceFormatCandidate {
    SurfaceFormat format = SurfaceFormat::Undefined;
    SurfaceColorSpace color_space = SurfaceColorSpace::SrgbNonlinear;
};

// Maps a selected candidate to the RHI texture format. Never called for `Undefined`/`Other`.
[[nodiscard]] constexpr rhi::TextureFormat to_rhi_format(SurfaceFormat format) {
    switch (format) {
    case SurfaceFormat::R8G8B8A8Srgb:
        return rhi::TextureFormat::R8G8B8A8Srgb;
    case SurfaceFormat::B8G8R8A8Srgb:
        return rhi::TextureFormat::B8G8R8A8Srgb;
    case SurfaceFormat::B8G8R8A8Unorm:
        return rhi::TextureFormat::B8G8R8A8Unorm;
    case SurfaceFormat::R8G8B8A8Unorm:
    case SurfaceFormat::Undefined:
    case SurfaceFormat::Other:
        break;
    }
    return rhi::TextureFormat::R8G8B8A8Unorm;
}

namespace detail {

struct PreferredSurfaceFormat {
    SurfaceFormat format;
};

// Presentation color policy (issue #791): UNORM first, then the sRGB siblings.
//
// The paint pipeline is sRGB-*encoded* internally: every color the stylesheet authored
// (`#2563eb` -> 37/99/235) reaches the shader as `byte / 255` and the blend happens in that encoded
// space (D2, docs/design/rendering-production-plan.md). An `*_SRGB` attachment applies the
// linear->sRGB transfer function on write, which encodes those already-encoded values a second
// time: #0b1220 (11,18,32) reaches the screen as (59,75,99), #2563eb (37,99,235) as (106,167,246).
// Measured on the Android shell emulator and matching `linear_to_srgb(authored/255)` exactly.
//
// So a presentation surface must be UNORM. The sRGB formats stay in the RHI for the paths that
// really do store encoded bytes and want hardware decoding on sample (an authored sRGB image or an
// SVG raster drawn into an sRGB target). Keeping the UNORM pair first and the sRGB pair as fallback
// keeps the list explicit and testable.
inline constexpr PreferredSurfaceFormat kPreferredSurfaceFormats[] = {
    {SurfaceFormat::R8G8B8A8Unorm},
    {SurfaceFormat::B8G8R8A8Unorm},
    {SurfaceFormat::R8G8B8A8Srgb},
    {SurfaceFormat::B8G8R8A8Srgb},
};

} // namespace detail

// Picks the swapchain surface format, returning the chosen candidate index:
// 1. the first preferred format (UNORM before sRGB, see above) advertised with an sRGB non-linear
//    color space;
// 2. the first preferred format with any color space, so a driver that pairs a preferred format
//    with an unusual color space still gets a usable swapchain;
// 3. a lone `Undefined` entry, which means "any format is fine": the caller then chooses UNORM.
// Returns nullopt when the surface offers no preferred format.
[[nodiscard]] constexpr std::optional<std::size_t>
choose_surface_format(std::span<const SurfaceFormatCandidate> formats) {
    for (const detail::PreferredSurfaceFormat& preferred : detail::kPreferredSurfaceFormats) {
        for (std::size_t i = 0; i < formats.size(); ++i) {
            if (formats[i].format == preferred.format &&
                formats[i].color_space == SurfaceColorSpace::SrgbNonlinear) {
                return i;
            }
        }
    }
    for (const detail::PreferredSurfaceFormat& preferred : detail::kPreferredSurfaceFormats) {
        for (std::size_t i = 0; i < formats.size(); ++i) {
            if (formats[i].format == preferred.format) {
                return i;
            }
        }
    }
    if (formats.size() == 1 && formats[0].format == SurfaceFormat::Undefined) {
        return std::size_t{0};
    }
    return std::nullopt;
}

} // namespace tessera::rendering::vulkan
