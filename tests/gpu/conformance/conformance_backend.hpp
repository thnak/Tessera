// SPDX-License-Identifier: MIT
//
// Backend selection for the GPU conformance suite (issue #161). Opening a backend never throws: a
// build without the Vulkan backend, or a host without a Vulkan device, yields an unavailable
// OpenedBackend whose reason lets the runner skip cleanly and keep CI without a GPU green.

#pragma once

#include "conformance_check.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace tessera::tests::gpu::conformance {

enum class BackendChoice { Null, Vulkan, Unknown };

[[nodiscard]] std::string_view backend_name(BackendChoice choice);
[[nodiscard]] BackendChoice parse_backend(std::string_view name);

// Which RHI capabilities the opened backend/device has. Until RHI Device::capabilities()
// (issue #160) lands, the backend half is its documented RHI support (both built backends
// implement buffers, targets, textures, pipelines, command lists and status) and the GPU-timestamp
// half is the device's own probe, which is the only authority for that fact.
struct BackendProfile {
    bool device = false;
    bool buffer_readback = false;
    bool buffer_bounds = false;
    bool render_target = false;
    bool texture = false;
    bool pipeline = false;
    bool command_list = false;
    bool device_status = false;
    bool gpu_timestamps = false;

    [[nodiscard]] bool has(Capability capability) const;
};

struct OpenedBackend {
    std::string name;
    std::unique_ptr<rendering::rhi::Device> device;
    BackendProfile profile;
    // Fixed, machine-independent text describing why no device is available. It reaches stdout and
    // the runner's skip line, never the summary.
    std::string unavailable_reason;

    [[nodiscard]] bool available() const {
        return device != nullptr;
    }
};

[[nodiscard]] OpenedBackend open_backend(BackendChoice choice);

// A pipeline descriptor the selected backend can create. Vulkan consumes SPIR-V, so it reuses the
// batched2d pipeline fixture; the null backend records the default descriptor.
[[nodiscard]] rendering::rhi::PipelineDesc pipeline_template(rendering::rhi::Backend backend);

} // namespace tessera::tests::gpu::conformance
