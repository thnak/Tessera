// SPDX-License-Identifier: MIT

#pragma once

// This and the sibling webgpu_*.cpp files compile only under Emscripten (the wasm-webgpu preset,
// emdawnwebgpu port pinned through tools/emsdk-version.txt) or with TESSERA_BUILD_WEBGPU and a
// native webgpu.h. See docs/design/gpu/webgpu-backend.md for what is verified.

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/webgpu/shader_registry.hpp"
#include "tessera/rendering/webgpu/webgpu_mapping.hpp"

#include <memory>
#include <vector>
#include <webgpu/webgpu.h>

namespace tessera::rendering::webgpu {

// WebGPU device and queue are created asynchronously by the browser host (adapter and device
// requests resolve in later event-loop turns), so the host owns that dance and hands over ready
// handles. The context keeps them alive for every resource the device creates.
struct WebGpuContext {
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    // Keyed by `PipelineDesc::shader_id`; starts with the built-in programs.
    ShaderRegistry shaders = builtin_shader_registry();
    bool lost = false;
    // Mip chain generation (RHI 1.6, issue #510): one downsample pipeline per render-target
    // format and the linear sampler every level is read with, created on first use.
    struct MipPipeline {
        WGPUTextureFormat format;
        WGPURenderPipeline pipeline;
    };
    std::vector<MipPipeline> mip_pipelines;
    WGPUSampler mip_sampler = nullptr;

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

// Builds levels 1..levels-1 of `texture` from level 0 with one render pass per level and submits
// them. Level 0 must already be written. Throws std::runtime_error if the pipeline cannot be made.
void generate_mip_chain(WebGpuContext& context, WGPUTexture texture, rhi::TextureFormat format,
                        std::uint32_t levels);

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
    [[nodiscard]] std::uint32_t levels() const override {
        return desc_.levels;
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

// What the command list needs from any target this backend hands out: the views a render pass
// attaches and a sampled bind reads. Offscreen targets and the browser canvas image both derive
// from it, so the command list casts to this one type instead of to a concrete class. Casting every
// target to `WebGpuRenderTarget` made a canvas target (a sibling, not a subclass) read the wrong
// memory and begin a pass with no view.
class WebGpuTargetViews : public rhi::RenderTarget {
public:
    [[nodiscard]] virtual WGPUTextureView color_view() const = 0;
    // Null for 1-sample targets.
    [[nodiscard]] virtual WGPUTextureView resolve_view() const = 0;
    [[nodiscard]] virtual WGPUTextureView sampled_view() const = 0;
    // Null for a target that cannot be sampled (the canvas image).
    [[nodiscard]] virtual WGPUSampler sampler() const = 0;
};

class WebGpuRenderTarget final : public WebGpuTargetViews {
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
    [[nodiscard]] rhi::Filter filter() const override {
        return desc_.filter;
    }
    [[nodiscard]] rhi::AddressMode address() const override {
        return desc_.address;
    }

    [[nodiscard]] WGPUTexture resolved_texture() const {
        return resolve_ != nullptr ? resolve_ : color_;
    }
    [[nodiscard]] WGPUTextureView color_view() const override {
        return color_view_;
    }
    [[nodiscard]] WGPUTextureView resolve_view() const override {
        return resolve_view_;
    }
    // RHI 1.3 sampled view (CommandList::bind_sampled_target): the target is read through its
    // 1-sample image - the resolve view for a 4x target - with the sampler it was created with.
    [[nodiscard]] WGPUTextureView sampled_view() const override {
        return resolve_view_ != nullptr ? resolve_view_ : color_view_;
    }
    [[nodiscard]] WGPUSampler sampler() const override {
        return sampler_;
    }

private:
    std::shared_ptr<WebGpuContext> context_;
    rhi::RenderTargetDesc desc_;
    WGPUTexture color_ = nullptr;
    WGPUTextureView color_view_ = nullptr;
    WGPUTexture resolve_ = nullptr;
    WGPUTextureView resolve_view_ = nullptr;
    // Created with the target (like a texture's sampler) so a sampled bind never allocates.
    WGPUSampler sampler_ = nullptr;
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
