// SPDX-License-Identifier: MIT
//
// The fault-injection enumerators for the Vulkan backend's test seam (issue #705 and the D7
// device-loss work). Split out of vulkan_internal.hpp, which is at its size baseline.
//
// The "no fault" enumerator is `NoFault`, not `None`: X11's X.h defines `None` as a macro, so
// `None` would break this header on every Linux Xlib build.

#pragma once

#include <cstdint>

namespace tessera::rendering::vulkan {

enum class InjectedDeviceFault : std::uint8_t {
    NoFault,
    DeviceLostOnSubmit,
    DeviceLostOnFenceWait,
    DeviceLostOnAcquire,
    DeviceLostOnPresent,
    // Out-of-device-memory on the next texture creation (issue #705). Distinct from the loss
    // faults: an OOM is recoverable, so the device stays Ready and the caller is expected to evict
    // and retry rather than tear the device down.
    OutOfMemoryOnCreateTexture,
};

} // namespace tessera::rendering::vulkan
