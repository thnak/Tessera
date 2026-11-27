// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::rendering::rhi {

// Device health for the D7 production policy. A loss is recoverable backend state, not an
// exception: operations that can report it (swapchain acquire/present, command `wait()`) return an
// error/false and the caller polls `status()`. After a loss every RHI object created before it is
// invalid and must be destroyed and recreated (the backend generation-guards destructors so a late
// destruction is safe); the device itself can be recreated with `recover()`.
enum class DeviceStatus : std::uint8_t { Ready, Lost };

enum class DeviceRecoveryError : std::uint8_t {
    // The backend has no recovery path (null/software backends).
    NotSupported,
    // `recover()` was called while the device is healthy.
    NotLost,
    // This loss was already recovered once and failed; a new loss is needed (one-shot policy).
    AlreadyAttempted,
    // RHI objects from the lost generation are still alive. Destroy them, then retry; this attempt
    // does not consume the one-shot budget.
    ResourcesOutstanding,
    // Device recreation failed (driver/device unavailable). The device stays Lost.
    DeviceCreationFailed,
};

} // namespace tessera::rendering::rhi
