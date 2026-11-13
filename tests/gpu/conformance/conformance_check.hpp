// SPDX-License-Identifier: MIT
//
// Check model for the backend-selectable GPU conformance suite (issue #161). A check is data: a
// stable name, the RHI capability it needs, and a function that runs against an already-created
// device. The suite, not the check, decides whether the capability is available, so a backend
// without a feature reports a skip that names the capability instead of a failure.

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <span>
#include <string>
#include <string_view>

namespace tessera::tests::gpu::conformance {

enum class Status { Pass, Skip, Fail };

// RHI contract areas a check can depend on. Availability is decided by the suite from the opened
// backend/device. RHI Device::capabilities() (issue #160) becomes the authoritative source when it
// lands; this enum is the seam it plugs into.
enum class Capability {
    Device,
    BufferReadback,
    BufferBounds,
    RenderTarget,
    Texture,
    Pipeline,
    CommandList,
    DeviceStatus,
    GpuTimestamps,
};

[[nodiscard]] std::string_view capability_name(Capability capability);
[[nodiscard]] std::string_view status_name(Status status);

struct CheckResult {
    Status status = Status::Pass;
    // Empty for a pass; a skip reason or a failure detail otherwise. This string is serialized into
    // the deterministic summary, so it must not carry timestamps, pointers or absolute paths.
    std::string detail;

    [[nodiscard]] static CheckResult pass();
    [[nodiscard]] static CheckResult skip(std::string reason);
    [[nodiscard]] static CheckResult fail(std::string message);
};

struct Check {
    std::string_view name;
    Capability capability = Capability::Device;
    CheckResult (*run)(rendering::rhi::Device& device) = nullptr;
};

// The suite in its declaration order; conformance_suite.cpp sorts by name before running, so the
// summary is deterministic regardless of this order.
[[nodiscard]] std::span<const Check> checks();

} // namespace tessera::tests::gpu::conformance
