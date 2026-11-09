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

// D2 color policy order: sRGB-encoded formats first, then the pre-D2 UNORM order. Keeping the
// sRGB/UNORM siblings adjacent makes the fallback list explicit and testable.
inline constexpr PreferredSurfaceFormat kPreferredSurfaceFormats[] = {
    {SurfaceFormat::R8G8B8A8Srgb},
    {SurfaceFormat::B8G8R8A8Srgb},
    {SurfaceFormat::R8G8B8A8Unorm},
    {SurfaceFormat::B8G8R8A8Unorm},
};

} // namespace detail

// Picks the swapchain surface format (D2 color policy), returning the chosen candidate index:
// 1. the first preferred format (sRGB before UNORM) advertised with an sRGB non-linear color space;
// 2. the first preferred format with any color space, so a driver that pairs a preferred format
//    with an unusual color space still gets a usable swapchain;
// 3. a lone `Undefined` entry, which means "any format is fine": the caller then chooses sRGB for
//    an sRGB color space and UNORM otherwise.
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
