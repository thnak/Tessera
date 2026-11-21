// SPDX-License-Identifier: MIT

// Built only by the wasm-webgpu preset (Emscripten); see webgpu_internal.hpp.

#include "webgpu_internal.hpp"

#include <stdexcept>
#include <utility>

namespace tessera::rendering::webgpu {
namespace {

WGPUTexture create_texture_2d(WebGpuContext& context, rhi::Extent2D extent,
                              rhi::TextureFormat format, std::uint32_t sample_count,
                              std::uint32_t usage, std::uint32_t levels = 1) {
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.usage = usage;
    desc.dimension = WGPUTextureDimension_2D;
    desc.size = {extent.width, extent.height, 1};
    desc.format = to_wgpu(texture_format(format));
    desc.mipLevelCount = levels;
    desc.sampleCount = sample_count;
    WGPUTexture texture = wgpuDeviceCreateTexture(context.device, &desc);
    if (texture == nullptr) {
        throw std::runtime_error("WebGPU: texture creation failed");
    }
    return texture;
}

} // namespace

WebGpuBuffer::WebGpuBuffer(std::shared_ptr<WebGpuContext> context, const rhi::BufferDesc& desc)
    : context_(std::move(context)),
      size_(desc.size) {
    WGPUBufferDescriptor wgpu_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    wgpu_desc.usage = buffer_usage_flags(desc.usage);
    // writeBuffer and copies need 4-byte multiples; the RHI-visible size stays as requested.
    wgpu_desc.size = align_up(desc.size, 4);
    buffer_ = wgpuDeviceCreateBuffer(context_->device, &wgpu_desc);
    if (buffer_ == nullptr) {
        throw std::runtime_error("WebGPU: buffer creation failed");
    }
}

WebGpuBuffer::~WebGpuBuffer() {
    if (buffer_ != nullptr) {
        wgpuBufferDestroy(buffer_);
        wgpuBufferRelease(buffer_);
    }
}

void WebGpuBuffer::write(const void* data, std::size_t size, std::size_t offset) {
    if (size > size_ || offset > size_ - size) {
        throw std::out_of_range("WebGpuBuffer: range exceeds the buffer size");
    }
    // Queue writes require 4-byte aligned offset and size. Unaligned partial writes need a staging
    // copy; reject them explicitly until that slice lands rather than corrupting neighbors.
    if (offset % 4 != 0 || size % 4 != 0) {
        throw std::invalid_argument("WebGpuBuffer: write offset and size must be multiples of 4");
    }
    wgpuQueueWriteBuffer(context_->queue, buffer_, offset, data, size);
}

void WebGpuBuffer::read(void*, std::size_t, std::size_t) const {
    throw std::runtime_error("WebGpuBuffer: synchronous readback is unavailable in the browser");
}

WebGpuTexture::WebGpuTexture(std::shared_ptr<WebGpuContext> context, const rhi::TextureDesc& desc)
    : context_(std::move(context)),
      desc_(desc) {
    if (!is_supported_sample_count(desc.sample_count)) {
        throw std::invalid_argument("WebGpuTexture: unsupported sample count");
    }
    if (desc.sample_count > 1 && desc.pixels != nullptr) {
        throw std::invalid_argument("WebGpuTexture: pixels cannot be uploaded to a 4x texture");
    }
    if (!rhi::valid_texture_levels(desc)) {
        throw std::invalid_argument("WebGpuTexture: invalid mip level count");
    }
    desc_.pixels = nullptr;
    // A chain is rendered level by level, so every level is also a render attachment.
    std::uint32_t usage =
        desc.sample_count > 1 ? texture_usage_bits::kRenderAttachment : sampled_texture_usage();
    if (desc.levels > 1) {
        usage |= texture_usage_bits::kRenderAttachment;
    }
    texture_ = create_texture_2d(*context_, desc.extent, desc.format, desc.sample_count, usage,
                                 desc.levels);
    // The default view spans every level, which is what the sampler selects from.
    view_ = wgpuTextureCreateView(texture_, nullptr);

    WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampler_desc.magFilter = to_wgpu(filter_mode(desc.filter));
    sampler_desc.minFilter = sampler_desc.magFilter;
    sampler_desc.addressModeU = to_wgpu(address_mode(desc.address));
    sampler_desc.addressModeV = sampler_desc.addressModeU;
    sampler_desc.addressModeW = sampler_desc.addressModeU;
    // A single-level texture never samples another level; a chain picks one from the draw's
    // minification, blending neighbours only when the texture itself is filtered linearly.
    sampler_desc.mipmapFilter = desc.filter == rhi::Filter::Linear ? WGPUMipmapFilterMode_Linear
                                                                   : WGPUMipmapFilterMode_Nearest;
    sampler_desc.lodMinClamp = 0.0f;
    sampler_desc.lodMaxClamp = static_cast<float>(desc.levels - 1U);
    sampler_ = wgpuDeviceCreateSampler(context_->device, &sampler_desc);

    if (desc.pixels != nullptr) {
        const std::size_t row_pitch =
            desc.row_pitch != 0
                ? desc.row_pitch
                : static_cast<std::size_t>(desc.extent.width) * bytes_per_pixel(desc.format);
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = texture_;
        WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        layout.bytesPerRow = static_cast<std::uint32_t>(row_pitch);
        layout.rowsPerImage = desc.extent.height;
        const WGPUExtent3D extent{desc.extent.width, desc.extent.height, 1};
        wgpuQueueWriteTexture(context_->queue, &dst, desc.pixels, row_pitch * desc.extent.height,
                              &layout, &extent);
        if (desc.levels > 1) {
            generate_mip_chain(*context_, texture_, desc.format, desc.levels);
        }
    }
}

WebGpuTexture::~WebGpuTexture() {
    if (sampler_ != nullptr) {
        wgpuSamplerRelease(sampler_);
    }
    if (view_ != nullptr) {
        wgpuTextureViewRelease(view_);
    }
    if (texture_ != nullptr) {
        wgpuTextureDestroy(texture_);
        wgpuTextureRelease(texture_);
    }
}

void WebGpuTexture::write(const void* data, std::size_t size) {
    if (desc_.levels > 1) {
        throw std::invalid_argument("WebGpuTexture: mipmapped textures are immutable");
    }
    if (desc_.sample_count > 1) {
        throw std::logic_error("WebGpuTexture: cannot upload to a multisampled texture");
    }
    const std::size_t row_pitch =
        static_cast<std::size_t>(desc_.extent.width) * bytes_per_pixel(desc_.format);
    if (size < row_pitch * desc_.extent.height) {
        throw std::out_of_range("WebGpuTexture: upload is smaller than the full image");
    }
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = texture_;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = static_cast<std::uint32_t>(row_pitch);
    layout.rowsPerImage = desc_.extent.height;
    const WGPUExtent3D extent{desc_.extent.width, desc_.extent.height, 1};
    wgpuQueueWriteTexture(context_->queue, &dst, data, size, &layout, &extent);
}

void WebGpuTexture::write_region(const rhi::TextureRegion& region, const void* data,
                                 std::size_t row_pitch) {
    if (desc_.levels > 1) {
        throw std::invalid_argument("WebGpuTexture: mipmapped textures are immutable");
    }
    if (desc_.sample_count > 1) {
        throw std::logic_error("WebGpuTexture: cannot upload to a multisampled texture");
    }
    const std::size_t pitch =
        rhi::checked_region_pitch(desc_.extent, bytes_per_pixel(desc_.format), region, row_pitch);
    if (region.empty()) {
        return;
    }
    if (data == nullptr) {
        throw std::invalid_argument("WebGpuTexture: region upload without data");
    }
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = texture_;
    dst.origin = {region.x, region.y, 0};
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = static_cast<std::uint32_t>(pitch);
    layout.rowsPerImage = region.height;
    const WGPUExtent3D extent{region.width, region.height, 1};
    // The data size WebGPU validates is the last row's bytes plus the rows before it at the pitch,
    // so a pitched source is not required to carry the padding after its last row.
    const std::size_t row_bytes =
        static_cast<std::size_t>(region.width) * bytes_per_pixel(desc_.format);
    const std::size_t size = pitch * (region.height - 1) + row_bytes;
    wgpuQueueWriteTexture(context_->queue, &dst, data, size, &layout, &extent);
}

WebGpuRenderTarget::WebGpuRenderTarget(std::shared_ptr<WebGpuContext> context,
                                       const rhi::RenderTargetDesc& desc)
    : context_(std::move(context)),
      desc_(desc) {
    if (!is_supported_sample_count(desc.sample_count)) {
        throw std::invalid_argument("WebGpuRenderTarget: unsupported sample count");
    }
    color_ = create_texture_2d(*context_, desc.extent, desc.format, desc.sample_count,
                               render_target_usage(desc.sample_count));
    color_view_ = wgpuTextureCreateView(color_, nullptr);
    if (desc.sample_count > 1) {
        resolve_ =
            create_texture_2d(*context_, desc.extent, desc.format, 1, render_target_usage(1));
        resolve_view_ = wgpuTextureCreateView(resolve_, nullptr);
    }
    // RHI 1.3: the sampler a sampled bind reads the target with, built once from the target's
    // filter/address (the same pair TextureDesc carries) so bind_sampled_target allocates nothing.
    WGPUSamplerDescriptor sampler_desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sampler_desc.magFilter = to_wgpu(filter_mode(desc.filter));
    sampler_desc.minFilter = sampler_desc.magFilter;
    sampler_desc.addressModeU = to_wgpu(address_mode(desc.address));
    sampler_desc.addressModeV = sampler_desc.addressModeU;
    sampler_desc.addressModeW = sampler_desc.addressModeU;
    sampler_ = wgpuDeviceCreateSampler(context_->device, &sampler_desc);
}

WebGpuRenderTarget::~WebGpuRenderTarget() {
    for (WGPUTextureView view : {resolve_view_, color_view_}) {
        if (view != nullptr) {
            wgpuTextureViewRelease(view);
        }
    }
    for (WGPUTexture texture : {resolve_, color_}) {
        if (texture != nullptr) {
            wgpuTextureDestroy(texture);
            wgpuTextureRelease(texture);
        }
    }
    if (sampler_ != nullptr) {
        wgpuSamplerRelease(sampler_);
    }
}

} // namespace tessera::rendering::webgpu
