// SPDX-License-Identifier: MIT

#pragma once

// UNBUILT on the authoring host: this and the sibling webgpu_*.cpp files compile only under
// Emscripten (or TESSERA_BUILD_WEBGPU with a webgpu.h on the include path). See
// docs/design/gpu/webgpu-backend.md for what is verified and how to verify the rest.

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/webgpu/shader_registry.hpp"
#include "tessera/rendering/webgpu/webgpu_mapping.hpp"

#include <memory>
#include <webgpu/webgpu.h>

namespace tessera::rendering::webgpu {

// WebGPU device and queue are created asynchronously by the browser host (adapter and device
// requests resolve in later event-loop turns), so the host owns that dance and hands over ready
// handles. The context keeps them alive for every resource the device creates.
struct WebGpuContext {
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    ShaderRegistry shaders;
    bool lost = false;

    WebGpuContext() = default;
    WebGpuContext(const WebGpuContext&) = delete;
    WebGpuContext& operator=(const WebGpuContext&) = delete;
    ~WebGpuContext();
};

// Enum conversions: switches over the pure mapping enums (webgpu.h numbering is not relied on).
[[nodiscard]] WGPUTextureFormat to_wgpu(WgpuTextureFormat format);
[[nodiscard]] WGPUVertexFormat to_wgpu(WgpuVertexFormat format);
[[nodiscard]] WGPUBlendFactor to_wgpu(WgpuBlendFactor factor);
[[nodiscard]] WGPULoadOp to_wgpu(WgpuLoadOp op);
[[nodiscard]] WGPUStoreOp to_wgpu(WgpuStoreOp op);
[[nodiscard]] WGPUFilterMode to_wgpu(WgpuFilterMode mode);
[[nodiscard]] WGPUAddressMode to_wgpu(WgpuAddressMode mode);

[[nodiscard]] inline WGPUStringView wgpu_string(std::string_view text) {
    return WGPUStringView{text.data(), text.size()};
}

class WebGpuBuffer final : public rhi::Buffer {
public:
    WebGpuBuffer(std::shared_ptr<WebGpuContext> context, const rhi::BufferDesc& desc);
    ~WebGpuBuffer() override;

    [[nodiscard]] std::size_t size() const override {
        return size_;
    }
    void write(const void* data, std::size_t size, std::size_t offset) override;
    // Browsers cannot map synchronously; readback goes through an async map the caller polls
    // (not in this slice). Throws std::runtime_error.
    void read(void* data, std::size_t size, std::size_t offset) const override;

    [[nodiscard]] WGPUBuffer handle() const {
        return buffer_;
    }

private:
    std::shared_ptr<WebGpuContext> context_;
    WGPUBuffer buffer_ = nullptr;
    std::size_t size_ = 0;
};

class WebGpuTexture final : public rhi::Texture {
public:
    WebGpuTexture(std::shared_ptr<WebGpuContext> context, const rhi::TextureDesc& desc);
    ~WebGpuTexture() override;

    [[nodiscard]] rhi::Extent2D extent() const override {
        return desc_.extent;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return desc_.format;
    }
    [[nodiscard]] std::uint32_t sample_count() const override {
        return desc_.sample_count;
    }
    void write(const void* data, std::size_t size) override;

    [[nodiscard]] WGPUTextureView view() const {
        return view_;
    }
    [[nodiscard]] WGPUSampler sampler() const {
        return sampler_;
    }

private:
    std::shared_ptr<WebGpuContext> context_;
    rhi::TextureDesc desc_;
    WGPUTexture texture_ = nullptr;
    WGPUTextureView view_ = nullptr;
    WGPUSampler sampler_ = nullptr;
};

class WebGpuRenderTarget final : public rhi::RenderTarget {
public:
    WebGpuRenderTarget(std::shared_ptr<WebGpuContext> context, const rhi::RenderTargetDesc& desc);
    ~WebGpuRenderTarget() override;

    [[nodiscard]] rhi::Extent2D extent() const override {
        return desc_.extent;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return desc_.format;
    }
    [[nodiscard]] std::uint32_t sample_count() const override {
        return desc_.sample_count;
    }

    [[nodiscard]] WGPUTexture resolved_texture() const {
        return resolve_ != nullptr ? resolve_ : color_;
    }
    [[nodiscard]] WGPUTextureView color_view() const {
        return color_view_;
    }
    // Null for 1-sample targets.
    [[nodiscard]] WGPUTextureView resolve_view() const {
        return resolve_view_;
    }

private:
    std::shared_ptr<WebGpuContext> context_;
    rhi::RenderTargetDesc desc_;
    WGPUTexture color_ = nullptr;
    WGPUTextureView color_view_ = nullptr;
    WGPUTexture resolve_ = nullptr;
    WGPUTextureView resolve_view_ = nullptr;
};

class WebGpuPipeline final : public rhi::Pipeline {
public:
    WebGpuPipeline(std::shared_ptr<WebGpuContext> context, const rhi::PipelineDesc& desc);
    ~WebGpuPipeline() override;

    [[nodiscard]] std::uint32_t sample_count() const override {
        return sample_count_;
    }
    [[nodiscard]] WGPURenderPipeline handle() const {
        return pipeline_;
    }
    [[nodiscard]] WGPUBindGroupLayout bind_group_layout() const {
        return bind_group_layout_;
    }
    [[nodiscard]] std::uint32_t uniform_size() const {
        return uniform_size_;
    }

private:
    std::shared_ptr<WebGpuContext> context_;
    WGPUBindGroupLayout bind_group_layout_ = nullptr;
    WGPUPipelineLayout layout_ = nullptr;
    WGPURenderPipeline pipeline_ = nullptr;
    std::uint32_t sample_count_ = 1;
    std::uint32_t uniform_size_ = 0;
};

[[nodiscard]] std::unique_ptr<rhi::CommandList>
make_command_list(std::shared_ptr<WebGpuContext> context);

} // namespace tessera::rendering::webgpu
