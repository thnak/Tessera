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
inline constexpr std::uint32_t kRhiVersionMajor = 1;
inline constexpr std::uint32_t kRhiVersionMinor = 1;
inline constexpr std::uint32_t kRhiVersion = (kRhiVersionMajor << 16) | kRhiVersionMinor;

} // namespace tessera::rendering::rhi
