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

enum class BackendChoice { Null, Vulkan, WebGpu, Metal, Unknown };

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
    // RHI 1.12 (ADR-068): the device's own `supports_storage_buffers()` answer, which is the only
    // authority for the capability.
    bool storage_buffer = false;

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
// batched2d pipeline fixture; Metal consumes MSL, so it names the same shader by id; the null
// backend records the default descriptor.
[[nodiscard]] rendering::rhi::PipelineDesc pipeline_template(rendering::rhi::Backend backend);

// Fills `desc` with the backend's storage-buffer probe program (RHI 1.12, ADR-068 slice 1), keeping
// the caller's `storage_buffer`/`color_format`/`blend` choices. Returns false when this backend has
// no such program, so the check reports a skip instead of failing on a pipeline it cannot build.
// The null backend needs no program: it records the descriptor, so it returns true unchanged.
[[nodiscard]] bool fill_storage_probe_pipeline(rendering::rhi::Backend backend,
                                               rendering::rhi::PipelineDesc& desc);

} // namespace tessera::tests::gpu::conformance
