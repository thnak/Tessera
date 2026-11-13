// SPDX-License-Identifier: MIT

#include "conformance_backend.hpp"
#include "tessera/rendering/null/null_device.hpp"
#if defined(TESSERA_TEST_HAS_VULKAN)
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#endif

#include <exception>
#include <utility>

namespace tessera::tests::gpu::conformance {
namespace {

using namespace tessera::rendering;

BackendProfile profile_for(const rhi::Device& device) {
    BackendProfile profile;
    profile.device = true;
    profile.buffer_readback = true;
    profile.buffer_bounds = true;
    profile.render_target = true;
    profile.texture = true;
    profile.pipeline = true;
    profile.command_list = true;
    profile.device_status = true;
    profile.gpu_timestamps = device.timestamp_capability().usable();
    return profile;
}

OpenedBackend open_null() {
    OpenedBackend opened;
    opened.name = "null";
    opened.device = null::create_null_device(rhi::DeviceDesc{});
    if (opened.device == nullptr || opened.device->backend() != rhi::Backend::Null) {
        opened.device.reset();
        opened.unavailable_reason = "the null backend did not return a null device";
        return opened;
    }
    opened.profile = profile_for(*opened.device);
    return opened;
}

OpenedBackend open_vulkan() {
    OpenedBackend opened;
    opened.name = "vulkan";
#if defined(TESSERA_TEST_HAS_VULKAN)
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::Vulkan;
    desc.validation = false;
    try {
        opened.device = vulkan::create_vulkan_device(desc);
    } catch (const std::exception&) {
        opened.device.reset();
        opened.unavailable_reason = "no Vulkan device could be created";
        return opened;
    }
    if (opened.device == nullptr || opened.device->backend() != rhi::Backend::Vulkan) {
        opened.device.reset();
        opened.unavailable_reason = "the Vulkan backend did not return a Vulkan device";
        return opened;
    }
    opened.profile = profile_for(*opened.device);
#else
    opened.unavailable_reason = "the Vulkan backend is not part of this build";
#endif
    return opened;
}

OpenedBackend open_webgpu() {
    OpenedBackend opened;
    opened.name = "webgpu";
    // The WebGPU device wraps a host-provided WGPUDevice/WGPUQueue (browser canvas or a native
    // Dawn/wgpu instance). This headless conformance runner owns no such host, so the arm is
    // selectable but reports a fixed reason instead of pretending to have a device. When a host
    // integration lands (webgpu.yml), it passes the handles through and this branch grows a real
    // creation path.
#if defined(TESSERA_TEST_HAS_WEBGPU)
    opened.unavailable_reason =
        "the WebGPU backend needs a host-provided WGPUDevice; this runner has no browser or native "
        "host";
#else
    opened.unavailable_reason = "the WebGPU backend is not part of this build";
#endif
    return opened;
}

} // namespace

std::string_view backend_name(BackendChoice choice) {
    switch (choice) {
    case BackendChoice::Null:
        return "null";
    case BackendChoice::Vulkan:
        return "vulkan";
    case BackendChoice::WebGpu:
        return "webgpu";
    case BackendChoice::Unknown:
        break;
    }
    return "unknown";
}

BackendChoice parse_backend(std::string_view name) {
    if (name == "null") {
        return BackendChoice::Null;
    }
    if (name == "vulkan") {
        return BackendChoice::Vulkan;
    }
    if (name == "webgpu") {
        return BackendChoice::WebGpu;
    }
    return BackendChoice::Unknown;
}

bool BackendProfile::has(Capability capability) const {
    switch (capability) {
    case Capability::Device:
        return device;
    case Capability::BufferReadback:
        return buffer_readback;
    case Capability::BufferBounds:
        return buffer_bounds;
    case Capability::RenderTarget:
        return render_target;
    case Capability::Texture:
        return texture;
    case Capability::Pipeline:
        return pipeline;
    case Capability::CommandList:
        return command_list;
    case Capability::DeviceStatus:
        return device_status;
    case Capability::GpuTimestamps:
        return gpu_timestamps;
    }
    return false;
}

OpenedBackend open_backend(BackendChoice choice) {
    switch (choice) {
    case BackendChoice::Null:
        return open_null();
    case BackendChoice::Vulkan:
        return open_vulkan();
    case BackendChoice::WebGpu:
        return open_webgpu();
    case BackendChoice::Unknown:
        break;
    }
    OpenedBackend opened;
    opened.name = "unknown";
    opened.unavailable_reason = "the requested backend is not known";
    return opened;
}

rhi::PipelineDesc pipeline_template(rhi::Backend backend) {
#if defined(TESSERA_TEST_HAS_VULKAN)
    if (backend == rhi::Backend::Vulkan) {
        return vulkan::batched2d_pipeline_desc();
    }
#endif
    static_cast<void>(backend);
    return rhi::PipelineDesc{};
}

} // namespace tessera::tests::gpu::conformance
