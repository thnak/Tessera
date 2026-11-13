// SPDX-License-Identifier: MIT
//
// Stable names and result constructors for the conformance check model (issue #161). These are the
// strings the deterministic summary serializes, so they live in one place and never depend on a
// backend.

#include "conformance_check.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace tessera::tests::gpu::conformance {

std::string_view capability_name(Capability capability) {
    switch (capability) {
    case Capability::Device:
        return "device";
    case Capability::BufferReadback:
        return "buffer_readback";
    case Capability::BufferBounds:
        return "buffer_bounds";
    case Capability::RenderTarget:
        return "render_target";
    case Capability::Texture:
        return "texture";
    case Capability::Pipeline:
        return "pipeline";
    case Capability::CommandList:
        return "command_list";
    case Capability::DeviceStatus:
        return "device_status";
    case Capability::GpuTimestamps:
        return "gpu_timestamps";
    }
    return "unknown";
}

std::string_view status_name(Status status) {
    switch (status) {
    case Status::Pass:
        return "pass";
    case Status::Skip:
        return "skip";
    case Status::Fail:
        return "fail";
    }
    return "fail";
}

CheckResult CheckResult::pass() {
    return CheckResult{};
}

CheckResult CheckResult::skip(std::string reason) {
    return CheckResult{Status::Skip, std::move(reason)};
}

CheckResult CheckResult::fail(std::string message) {
    return CheckResult{Status::Fail, std::move(message)};
}

} // namespace tessera::tests::gpu::conformance
