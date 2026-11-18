// SPDX-License-Identifier: MIT

#include "conformance_backend.hpp"
#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/null/null_device.hpp"
#if defined(TESSERA_TEST_HAS_VULKAN)
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#endif
#if defined(TESSERA_TEST_HAS_METAL)
#include "tessera/rendering/metal/metal_device.hpp"
#include "tessera/rendering/metal/metal_shader_registry.hpp"
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

OpenedBackend open_metal() {
    OpenedBackend opened;
    opened.name = "metal";
#if defined(TESSERA_TEST_HAS_METAL)
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::Metal;
    desc.validation = false;
    try {
        opened.device = metal::create_metal_device(desc);
    } catch (const std::exception&) {
        opened.device.reset();
        opened.unavailable_reason = "no Metal device could be created";
        return opened;
    }
    if (opened.device == nullptr || opened.device->backend() != rhi::Backend::Metal) {
        opened.device.reset();
        opened.unavailable_reason = "the Metal backend did not return a Metal device";
        return opened;
    }
    opened.profile = profile_for(*opened.device);
    // The MSL shader table is built only when the build embedded cross-compiled MSL. Without it a
    // pipeline cannot be created, so the profile says so instead of letting the pipeline check fail
    // with a confusing message.
    opened.profile.pipeline = metal::has_msl_shaders();
#else
    opened.unavailable_reason = "the Metal backend is not part of this build";
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
    case BackendChoice::Metal:
        return "metal";
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
    if (name == "metal") {
        return BackendChoice::Metal;
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
    case BackendChoice::Metal:
        return open_metal();
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
    if (backend == rhi::Backend::Metal) {
        // Metal consumes MSL, so the same program is named by its manifest id rather than by
        // SPIR-V words (ADR-049: one logical program, one stable shader_id). The vertex layout is
        // still required: the MSL declares `[[stage_in]]` attributes by location, and Metal
        // rejects a pipeline whose descriptor omits an attribute the shader reads.
        rhi::PipelineDesc desc;
        desc.shader_id = rendering::gpu::kBatched2dShaderId;
        desc.vertex_layout.stride = rendering::gpu::kVertex2DStride;
        desc.vertex_layout.attributes = rendering::gpu::kVertex2DAttributes;
        desc.vertex_layout.attribute_count = rendering::gpu::kVertex2DAttributeCount;
        desc.color_format = rhi::TextureFormat::R8G8B8A8Unorm;
        desc.blend = rhi::BlendMode::Alpha;
        desc.push_constant_size = sizeof(rendering::gpu::Batched2DPushConstants);
        return desc;
    }
    static_cast<void>(backend);
    return rhi::PipelineDesc{};
}

} // namespace tessera::tests::gpu::conformance
