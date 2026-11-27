// SPDX-License-Identifier: MIT

// Built only by the wasm-webgpu preset (Emscripten); see webgpu_internal.hpp.

#include "tessera/rendering/webgpu/webgpu_device.hpp"
#include "webgpu_internal.hpp"

#include <stdexcept>
#include <utility>

namespace tessera::rendering::webgpu {

static_assert(rhi::kRhiVersionMajor == 1, "WebGPU backend implements RHI 1.x");

WebGpuContext::~WebGpuContext() {
    for (const MipPipeline& cached : mip_pipelines) {
        wgpuRenderPipelineRelease(cached.pipeline);
    }
    if (mip_sampler != nullptr) {
        wgpuSamplerRelease(mip_sampler);
    }
    if (queue != nullptr) {
        wgpuQueueRelease(queue);
    }
    if (device != nullptr) {
        wgpuDeviceRelease(device);
    }
}

WGPUTextureFormat to_wgpu(WgpuTextureFormat format) {
    switch (format) {
    case WgpuTextureFormat::Rgba8Unorm:
        return WGPUTextureFormat_RGBA8Unorm;
    case WgpuTextureFormat::R8Unorm:
        return WGPUTextureFormat_R8Unorm;
    case WgpuTextureFormat::Bgra8Unorm:
        return WGPUTextureFormat_BGRA8Unorm;
    case WgpuTextureFormat::Rgba8UnormSrgb:
        return WGPUTextureFormat_RGBA8UnormSrgb;
    case WgpuTextureFormat::Bgra8UnormSrgb:
        return WGPUTextureFormat_BGRA8UnormSrgb;
    }
    return WGPUTextureFormat_RGBA8Unorm;
}

WGPUVertexFormat to_wgpu(WgpuVertexFormat format) {
    switch (format) {
    case WgpuVertexFormat::Float32:
        return WGPUVertexFormat_Float32;
    case WgpuVertexFormat::Float32x2:
        return WGPUVertexFormat_Float32x2;
    case WgpuVertexFormat::Float32x3:
        return WGPUVertexFormat_Float32x3;
    case WgpuVertexFormat::Float32x4:
        return WGPUVertexFormat_Float32x4;
    case WgpuVertexFormat::Unorm8x4:
        return WGPUVertexFormat_Unorm8x4;
    case WgpuVertexFormat::Uint32:
        // The per-vertex texture-array index (RHI 1.7, issue #480 part 2). WebGPU has no descriptor
        // indexing, so the WGSL twin ignores the value; the attribute still maps so one Vertex2D
        // serves both backends.
        return WGPUVertexFormat_Uint32;
    }
    return WGPUVertexFormat_Float32;
}

WGPUBlendFactor to_wgpu(WgpuBlendFactor factor) {
    switch (factor) {
    case WgpuBlendFactor::Zero:
        return WGPUBlendFactor_Zero;
    case WgpuBlendFactor::One:
        return WGPUBlendFactor_One;
    case WgpuBlendFactor::SrcAlpha:
        return WGPUBlendFactor_SrcAlpha;
    case WgpuBlendFactor::OneMinusSrcAlpha:
        return WGPUBlendFactor_OneMinusSrcAlpha;
    case WgpuBlendFactor::OneMinusDstAlpha:
        return WGPUBlendFactor_OneMinusDstAlpha;
    }
    return WGPUBlendFactor_One;
}

WGPULoadOp to_wgpu(WgpuLoadOp op) {
    return op == WgpuLoadOp::Load ? WGPULoadOp_Load : WGPULoadOp_Clear;
}

WGPUStoreOp to_wgpu(WgpuStoreOp op) {
    return op == WgpuStoreOp::Store ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
}

WGPUFilterMode to_wgpu(WgpuFilterMode mode) {
    return mode == WgpuFilterMode::Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
}

WGPUAddressMode to_wgpu(WgpuAddressMode mode) {
    return mode == WgpuAddressMode::Repeat ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge;
}

namespace {

class WebGpuDevice final : public rhi::Device {
public:
    explicit WebGpuDevice(std::shared_ptr<WebGpuContext> context)
        : context_(std::move(context)) {}

    [[nodiscard]] rhi::Backend backend() const override {
        return rhi::Backend::WebGpu;
    }

    [[nodiscard]] rhi::RhiCapabilities capabilities() const override {
        return rhi::RhiCapabilities{rhi::Backend::WebGpu, rhi::kRhiVersion, timestamp_capability()};
    }

    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc& desc) override {
        return std::make_unique<WebGpuBuffer>(context_, desc);
    }

    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override {
        return std::make_unique<WebGpuTexture>(context_, desc);
    }

    std::unique_ptr<rhi::Pipeline> create_pipeline(const rhi::PipelineDesc& desc) override {
        return std::make_unique<WebGpuPipeline>(context_, desc);
    }

    std::unique_ptr<rhi::RenderTarget>
    create_render_target(const rhi::RenderTargetDesc& desc) override {
        return std::make_unique<WebGpuRenderTarget>(context_, desc);
    }

    std::unique_ptr<rhi::CommandList> create_command_list() override {
        return make_command_list(context_);
    }

    // The canvas surface belongs to the Wasm host (backends.md section 4); a swapchain adapter over
    // wgpuSurface* is a later slice.
    [[nodiscard]] std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
    create_swapchain(const rhi::SwapchainDesc&) override {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }

    // The browser UI thread must never block, and WebGPU offers no synchronous wait. Completion is
    // observed through CommandList::is_complete().
    void wait_idle() override {}

    [[nodiscard]] bool supports_sample_count(rhi::TextureFormat,
                                             std::uint32_t sample_count) const override {
        // WebGPU guarantees 1x and 4x for the 8-bit color formats this RHI exposes.
        return is_supported_sample_count(sample_count);
    }

    // WebGPU exposes one fragment output per color target and no SRC1 blend factor, so the
    // per-channel coverage of `BlendMode::DualSource` has no expression here. Reporting false is
    // what routes LCD text to grayscale coverage on this backend (issue #471).
    [[nodiscard]] bool supports_dual_source_blend() const override {
        return false;
    }

    // The chain is a render-pass downsample (webgpu_mipmaps.cpp), so every format this backend
    // maps is covered: all five are renderable and filterable in core WebGPU.
    [[nodiscard]] bool supports_texture_mipmaps(rhi::TextureFormat) const override {
        return true;
    }

    // `read-only-storage` bindings are core WebGPU: bind_storage_buffer binds the buffer as binding
    // 3 of group 0 (ADR-068 slice 2). The batched 2D WGSL twin declares that binding
    // unconditionally, so the pipeline must be created with `storage_buffer`, which the renderer
    // does when this is true.
    [[nodiscard]] bool supports_storage_buffers() const override {
        return true;
    }

    // RHI 1.20 (ADR-076): batched2d.wgsl carries the gradient branch, read from the same storage
    // binding as the clip records.
    [[nodiscard]] bool supports_analytic_gradients() const override {
        return true;
    }

    [[nodiscard]] rhi::DeviceStatus status() const override {
        return context_->lost ? rhi::DeviceStatus::Lost : rhi::DeviceStatus::Ready;
    }

    [[nodiscard]] WebGpuContext& context() {
        return *context_;
    }

private:
    std::shared_ptr<WebGpuContext> context_;
};

} // namespace

std::unique_ptr<rhi::Device> create_device(void* wgpu_device, void* wgpu_queue) {
    if (wgpu_device == nullptr || wgpu_queue == nullptr) {
        throw std::invalid_argument("create_device: device and queue handles are required");
    }
    auto context = std::make_shared<WebGpuContext>();
    context->device = static_cast<WGPUDevice>(wgpu_device);
    context->queue = static_cast<WGPUQueue>(wgpu_queue);
    wgpuDeviceAddRef(context->device);
    wgpuQueueAddRef(context->queue);
    return std::make_unique<WebGpuDevice>(std::move(context));
}

bool register_shader(rhi::Device& device, std::string_view shader_id, const WgslProgram& program) {
    if (device.backend() != rhi::Backend::WebGpu) {
        return false;
    }
    // Backend tag checked above; the repo avoids RTTI, so no dynamic_cast.
    return static_cast<WebGpuDevice&>(device).context().shaders.add(shader_id, program);
}

} // namespace tessera::rendering::webgpu
