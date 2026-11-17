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
// 1.3 (2026-11-13): additive sampled view of an offscreen render target
// (`CommandList::bind_sampled_target`, `RenderTargetDesc::filter`/`address`), the capability the
// glass composite draws with (issue #254, docs/design/gpu/offscreen-layers.md section 5).
// 1.4 (2026-11-14): additive `DeviceDesc::surface` (`SurfacePlatform`) selecting the presentation
// surface family, so a build that can present through both Xlib and Wayland picks the one the HAL
// window belongs to (issue #155, platform(linux) Wayland window + Vulkan surface).
// 1.5 (2026-11-18): additive `BlendMode::DualSource` (per-channel coverage, `src0 * src1 + dst *
// (1 - src1)`) and the capability that gates it, `Device::supports_dual_source_blend()`. The
// capability is pure virtual because a backend that cannot answer it cannot honour the contract;
// the fallback is grayscale coverage, so no frame changes (issue #471 slice 1).
inline constexpr std::uint32_t kRhiVersionMajor = 1;
inline constexpr std::uint32_t kRhiVersionMinor = 5;
inline constexpr std::uint32_t kRhiVersion = (kRhiVersionMajor << 16) | kRhiVersionMinor;

} // namespace tessera::rendering::rhi
