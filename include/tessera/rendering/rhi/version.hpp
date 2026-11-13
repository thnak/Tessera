// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::rendering::rhi {

// Semantic version of the public RHI contract in include/tessera/rendering/rhi/** (ADR-049).
// Bump major for breaking changes, minor for additive compatible changes. Backends must
// declare the version they implement (static_assert in backend sources) and report it via
// Device::capabilities().
// 1.1 (2026-11-13): additive `CommandList::copy_target_region` for the isolated-layer seam
// (issue #242, docs/design/gpu/offscreen-layers.md).
// 1.2 (2026-11-13): additive swapchain composite-alpha request (`SwapchainDesc::per_pixel_alpha`)
// and the resolved-mode query (`Swapchain::alpha_mode`/`supports_per_pixel_alpha`), glass-v1.md
// section 6 (issue #243).
inline constexpr std::uint32_t kRhiVersionMajor = 1;
inline constexpr std::uint32_t kRhiVersionMinor = 2;
inline constexpr std::uint32_t kRhiVersion = (kRhiVersionMajor << 16) | kRhiVersionMinor;

} // namespace tessera::rendering::rhi
