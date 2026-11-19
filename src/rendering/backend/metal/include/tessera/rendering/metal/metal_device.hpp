// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <memory>

namespace tessera::rendering::metal {

// The native Metal backend's factory (ADR-053, docs/design/gpu/metal-backend.md).
//
// This header is plain C++ on purpose: no `id<...>`, no `NS*`, no `<Metal/Metal.h>`. Objective-C
// objects are held with ARC inside src/rendering/backend/metal/src/ and never appear in a header
// (docs/design/gpu/metal-backend.md section 4), so a caller above the RHI seam cannot accidentally
// depend on Metal.
//
// Only compiled when TESSERA_BUILD_METAL is ON, which requires an Apple platform. A caller that
// wants to be portable asks `rhi::DeviceDesc::backend == rhi::Backend::Metal` and handles the
// "not built in" case; there is no stub device, because a device that silently does nothing is
// worse than a missing symbol.

// Creates the Metal device. Throws std::runtime_error when no Metal device is available (a headless
// host, or a Mac whose GPU is unavailable) and std::invalid_argument when `desc.backend` is not
// `Backend::Metal`.
//
// `desc.validation` turns on Metal's API validation layer for the device's lifetime; it is a debug
// and test facility, never a production setting (docs/design/gpu/metal-backend.md section 6).
[[nodiscard]] std::unique_ptr<rhi::Device> create_metal_device(const rhi::DeviceDesc& desc);

// True when this process can create a Metal device at all. Cheap and side-effect free: it asks
// whether the system default device exists, and does not create one. A caller uses it to decide
// between the Metal backend and a fallback before committing to a device.
[[nodiscard]] bool metal_available();

} // namespace tessera::rendering::metal
