// SPDX-License-Identifier: MIT

// The native Metal device (ADR-053, docs/design/gpu/metal-backend.md).
//
// Objective-C++ with ARC, confined to this directory: no `id<...>` or `NS*` type leaves it. The
// device owns one MTLDevice, one MTLCommandQueue and one MTLSharedEvent that carries the completion
// value every submission signals (section 2 and 3.1 of the design note).
//
// This file is the device, buffer, texture and render-target half. Command recording, pipelines and
// the CAMetalLayer swapchain are separate translation units so each stays inside the file-size
// limit and one responsibility.

#include "metal_internal.hpp"

#include "tessera/rendering/metal/metal_device.hpp"

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace tessera::rendering::metal {

// The mapping table pins Metal's documented enumerator values so it can be tested without a Metal
// toolchain. These asserts are the other half of that bargain: on a Mac, the SDK's own enumerators
// must agree with the table, so a wrong number is a compile error here rather than a silently wrong
// pixel format at run time.
static_assert(static_cast<std::uint32_t>(PixelFormat::R8Unorm) == MTLPixelFormatR8Unorm);
static_assert(static_cast<std::uint32_t>(PixelFormat::R8UnormSrgb) == MTLPixelFormatR8Unorm_sRGB);
static_assert(static_cast<std::uint32_t>(PixelFormat::RGBA8Unorm) == MTLPixelFormatRGBA8Unorm);
static_assert(static_cast<std::uint32_t>(PixelFormat::RGBA8UnormSrgb) ==
              MTLPixelFormatRGBA8Unorm_sRGB);
static_assert(static_cast<std::uint32_t>(PixelFormat::BGRA8Unorm) == MTLPixelFormatBGRA8Unorm);
static_assert(static_cast<std::uint32_t>(PixelFormat::BGRA8UnormSrgb) ==
              MTLPixelFormatBGRA8Unorm_sRGB);

static_assert(static_cast<std::uint32_t>(VertexFormat::UChar4Normalized) ==
              MTLVertexFormatUChar4Normalized);
static_assert(static_cast<std::uint32_t>(VertexFormat::Float) == MTLVertexFormatFloat);
static_assert(static_cast<std::uint32_t>(VertexFormat::Float2) == MTLVertexFormatFloat2);
static_assert(static_cast<std::uint32_t>(VertexFormat::Float3) == MTLVertexFormatFloat3);
static_assert(static_cast<std::uint32_t>(VertexFormat::Float4) == MTLVertexFormatFloat4);
static_assert(static_cast<std::uint32_t>(VertexFormat::UInt) == MTLVertexFormatUInt);

static_assert(static_cast<std::uint32_t>(StorageMode::Shared) == MTLResourceStorageModeShared);
static_assert(static_cast<std::uint32_t>(StorageMode::Managed) == MTLResourceStorageModeManaged);
static_assert(static_cast<std::uint32_t>(StorageMode::Private) == MTLResourceStorageModePrivate);
static_assert(static_cast<std::uint32_t>(HazardTracking::Tracked) ==
              MTLResourceHazardTrackingModeTracked);

static_assert(static_cast<std::uint32_t>(LoadAction::DontCare) == MTLLoadActionDontCare);
static_assert(static_cast<std::uint32_t>(LoadAction::Load) == MTLLoadActionLoad);
static_assert(static_cast<std::uint32_t>(LoadAction::Clear) == MTLLoadActionClear);
static_assert(static_cast<std::uint32_t>(StoreAction::DontCare) == MTLStoreActionDontCare);
static_assert(static_cast<std::uint32_t>(StoreAction::Store) == MTLStoreActionStore);
static_assert(static_cast<std::uint32_t>(StoreAction::MultisampleResolve) ==
              MTLStoreActionMultisampleResolve);
static_assert(static_cast<std::uint32_t>(StoreAction::StoreAndMultisampleResolve) ==
              MTLStoreActionStoreAndMultisampleResolve);

static_assert(static_cast<std::uint32_t>(SamplerFilter::Nearest) == MTLSamplerMinMagFilterNearest);
static_assert(static_cast<std::uint32_t>(SamplerFilter::Linear) == MTLSamplerMinMagFilterLinear);
static_assert(static_cast<std::uint32_t>(SamplerAddressMode::ClampToEdge) ==
              MTLSamplerAddressModeClampToEdge);
static_assert(static_cast<std::uint32_t>(SamplerAddressMode::Repeat) ==
              MTLSamplerAddressModeRepeat);

static_assert(static_cast<std::uint32_t>(BlendFactor::Zero) == MTLBlendFactorZero);
static_assert(static_cast<std::uint32_t>(BlendFactor::One) == MTLBlendFactorOne);
static_assert(static_cast<std::uint32_t>(BlendFactor::SourceAlpha) == MTLBlendFactorSourceAlpha);
static_assert(static_cast<std::uint32_t>(BlendFactor::OneMinusSourceAlpha) ==
              MTLBlendFactorOneMinusSourceAlpha);
static_assert(static_cast<std::uint32_t>(BlendFactor::Source1Color) == MTLBlendFactorSource1Color);
static_assert(static_cast<std::uint32_t>(BlendFactor::OneMinusSource1Color) ==
              MTLBlendFactorOneMinusSource1Color);
static_assert(static_cast<std::uint32_t>(BlendOperation::Add) == MTLBlendOperationAdd);

// RHI 1.x is the contract this backend implements (docs/guides/writing-a-backend.md section 4).
static_assert(rhi::kRhiVersionMajor == 1, "the Metal backend implements RHI 1.x");

namespace {

// The device's completion value, published by Metal's completed handler and read by the retire
// queue. The handler runs on a thread Metal chooses, so it may only store an atomic here; it never
// frees anything and never touches scene state (docs/design/gpu/metal-backend.md section 3.1).
class SharedEventCompletion final : public rhi::CompletionSource {
public:
    [[nodiscard]] std::uint64_t completed_value() const override {
        return completed_.load(std::memory_order_acquire);
    }

    void publish(std::uint64_t value) {
        // Monotonic: a late handler for an older submission must not move the value backwards.
        std::uint64_t current = completed_.load(std::memory_order_relaxed);
        while (current < value &&
               !completed_.compare_exchange_weak(current, value, std::memory_order_release,
                                                 std::memory_order_relaxed)) {
        }
    }

private:
    std::atomic<std::uint64_t> completed_{0};
};

} // namespace

// --- device ------------------------------------------------------------------------------------

MetalDevice::MetalDevice(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLSharedEvent> event,
                         const rhi::DeviceDesc& desc)
    : device_(device),
      queue_(queue),
      event_(event),
      desc_(desc),
      completion_(std::make_unique<SharedEventCompletion>()) {}

rhi::Backend MetalDevice::backend() const {
    return rhi::Backend::Metal;
}

rhi::RhiCapabilities MetalDevice::capabilities() const {
    return rhi::RhiCapabilities{rhi::Backend::Metal, rhi::kRhiVersion, timestamp_capability()};
}

std::uint64_t MetalDevice::next_completion_value() {
    return ++next_value_;
}

void MetalDevice::publish_completed(std::uint64_t value) {
    static_cast<SharedEventCompletion&>(*completion_).publish(value);
}

const rhi::CompletionSource& MetalDevice::completion_source() const {
    return *completion_;
}

void MetalDevice::mark_lost() {
    status_ = rhi::DeviceStatus::Lost;
}

// --- buffers -----------------------------------------------------------------------------------

MetalBuffer::MetalBuffer(id<MTLBuffer> buffer, std::size_t size) : buffer_(buffer), size_(size) {}

std::size_t MetalBuffer::size() const {
    return size_;
}

void MetalBuffer::check_range(std::size_t size, std::size_t offset) const {
    if (offset > size_ || size > size_ - offset) {
        throw std::out_of_range("metal: buffer access outside the buffer");
    }
}

void MetalBuffer::write(const void* data, std::size_t size, std::size_t offset) {
    check_range(size, offset);
    if (size == 0) {
        return;
    }
    // Every RHI buffer use is created Shared (metal_mapping.cpp), so the contents pointer is
    // CPU-visible and a memcpy is the whole upload: no staging buffer, no blit, no
    // didModifyRange: (that is the Managed path, which this backend never takes).
    std::memcpy(static_cast<std::uint8_t*>(buffer_.contents) + offset, data, size);
}

void MetalBuffer::read(void* data, std::size_t size, std::size_t offset) const {
    check_range(size, offset);
    if (size == 0) {
        return;
    }
    std::memcpy(data, static_cast<const std::uint8_t*>(buffer_.contents) + offset, size);
}

// --- textures ----------------------------------------------------------------------------------

MetalTexture::MetalTexture(id<MTLTexture> texture, rhi::Extent2D extent, rhi::TextureFormat format,
                           std::uint32_t levels)
    : texture_(texture), extent_(extent), format_(format), levels_(levels) {}

rhi::Extent2D MetalTexture::extent() const {
    return extent_;
}

rhi::TextureFormat MetalTexture::format() const {
    return format_;
}

std::uint32_t MetalTexture::levels() const {
    return levels_;
}

void MetalTexture::write(const void* data, std::size_t size) {
    if (levels_ > 1) {
        // A mipmapped texture is immutable: rewriting the base level would leave the chain stale
        // (RHI 1.6, docs/design/gpu/rhi.md section 5.2).
        throw std::logic_error("metal: a mipmapped texture cannot be rewritten");
    }
    const std::size_t expected =
        static_cast<std::size_t>(extent_.width) * extent_.height * bytes_per_pixel(format_);
    if (size != expected) {
        throw std::invalid_argument("metal: texture upload size does not match the extent");
    }
    [texture_ replaceRegion:MTLRegionMake2D(0, 0, extent_.width, extent_.height)
                mipmapLevel:0
                  withBytes:data
                bytesPerRow:static_cast<NSUInteger>(extent_.width) * bytes_per_pixel(format_)];
}

// --- render targets ----------------------------------------------------------------------------

MetalRenderTarget::MetalRenderTarget(id<MTLTexture> color, id<MTLTexture> resolve,
                                     rhi::Extent2D extent, rhi::TextureFormat format,
                                     std::uint32_t sample_count, bool presentable,
                                     rhi::Filter filter, rhi::AddressMode address)
    : color_(color),
      resolve_(resolve),
      extent_(extent),
      format_(format),
      sample_count_(sample_count),
      presentable_(presentable),
      filter_(filter),
      address_(address) {}

rhi::Extent2D MetalRenderTarget::extent() const {
    return extent_;
}

rhi::TextureFormat MetalRenderTarget::format() const {
    return format_;
}

std::uint32_t MetalRenderTarget::sample_count() const {
    return sample_count_;
}

bool MetalRenderTarget::presentable() const {
    return presentable_;
}

rhi::Filter MetalRenderTarget::filter() const {
    return filter_;
}

rhi::AddressMode MetalRenderTarget::address() const {
    return address_;
}

// --- device creation ---------------------------------------------------------------------------

std::unique_ptr<rhi::Buffer> MetalDevice::create_buffer(const rhi::BufferDesc& desc) {
    if (desc.size == 0) {
        throw std::invalid_argument("metal: create_buffer needs a non-zero size");
    }
    const MTLResourceOptions options =
        static_cast<MTLResourceOptions>(static_cast<std::uint32_t>(buffer_storage_mode(desc.usage)) |
                                        static_cast<std::uint32_t>(HazardTracking::Tracked));
    id<MTLBuffer> buffer = [device_ newBufferWithLength:desc.size options:options];
    if (buffer == nil) {
        throw std::runtime_error("metal: newBufferWithLength failed");
    }
    return std::make_unique<MetalBuffer>(buffer, desc.size);
}

std::unique_ptr<rhi::Texture> MetalDevice::create_texture(const rhi::TextureDesc& desc) {
    if (desc.extent.width == 0 || desc.extent.height == 0) {
        throw std::invalid_argument("metal: create_texture needs a non-zero extent");
    }
    if (!rhi::valid_texture_levels(desc)) {
        throw std::invalid_argument("metal: invalid mip level count");
    }
    if (desc.levels > 1 && !supports_texture_mipmaps(desc.format)) {
        throw std::runtime_error("metal: mip chains are not supported for this format");
    }
    if (desc.sample_count != 1) {
        throw std::invalid_argument("metal: a texture is 1-sample; use a render target for MSAA");
    }

    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = static_cast<MTLPixelFormat>(pixel_format(desc.format));
    descriptor.width = desc.extent.width;
    descriptor.height = desc.extent.height;
    descriptor.mipmapLevelCount = desc.levels;
    descriptor.sampleCount = 1;
    descriptor.storageMode = static_cast<MTLStorageMode>(texture_storage_mode());
    descriptor.usage = MTLTextureUsageShaderRead;
    if (desc.levels > 1) {
        // The chain is built at creation from the uploaded base level, so the texture is also a
        // render target for the downsample passes (RHI 1.6).
        descriptor.usage |= MTLTextureUsageRenderTarget;
    }

    id<MTLTexture> texture = [device_ newTextureWithDescriptor:descriptor];
    if (texture == nil) {
        throw std::runtime_error("metal: newTextureWithDescriptor failed");
    }
    auto created = std::make_unique<MetalTexture>(texture, desc.extent, desc.format, desc.levels);
    if (desc.pixels != nullptr) {
        created->write(desc.pixels, static_cast<std::size_t>(desc.extent.width) * desc.extent.height *
                                        bytes_per_pixel(desc.format));
    }
    return created;
}

std::unique_ptr<rhi::RenderTarget> MetalDevice::create_render_target(
    const rhi::RenderTargetDesc& desc) {
    if (desc.extent.width == 0 || desc.extent.height == 0) {
        throw std::invalid_argument("metal: create_render_target needs a non-zero extent");
    }
    if (!rhi::is_supported_sample_count(desc.sample_count)) {
        throw std::invalid_argument("metal: unsupported sample count");
    }
    if (!supports_sample_count(desc.format, desc.sample_count)) {
        throw std::runtime_error("metal: sample count is not supported for this format");
    }

    MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
    descriptor.textureType = MTLTextureType2D;
    descriptor.pixelFormat = static_cast<MTLPixelFormat>(pixel_format(desc.format));
    descriptor.width = desc.extent.width;
    descriptor.height = desc.extent.height;
    descriptor.mipmapLevelCount = 1;
    descriptor.sampleCount = desc.sample_count;
    descriptor.storageMode = static_cast<MTLStorageMode>(texture_storage_mode());
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

    id<MTLTexture> color = [device_ newTextureWithDescriptor:descriptor];
    if (color == nil) {
        throw std::runtime_error("metal: newTextureWithDescriptor(render target) failed");
    }

    id<MTLTexture> resolve = nil;
    if (desc.sample_count > 1) {
        // A 4x target owns a multisample image plus a 1-sample resolve image; readback and
        // sampling observe the resolved image (RHI 1.0, device.hpp).
        MTLTextureDescriptor* resolve_descriptor = [[MTLTextureDescriptor alloc] init];
        resolve_descriptor.textureType = MTLTextureType2D;
        resolve_descriptor.pixelFormat = static_cast<MTLPixelFormat>(pixel_format(desc.format));
        resolve_descriptor.width = desc.extent.width;
        resolve_descriptor.height = desc.extent.height;
        resolve_descriptor.mipmapLevelCount = 1;
        resolve_descriptor.sampleCount = 1;
        resolve_descriptor.storageMode = static_cast<MTLStorageMode>(texture_storage_mode());
        resolve_descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        resolve = [device_ newTextureWithDescriptor:resolve_descriptor];
        if (resolve == nil) {
            throw std::runtime_error("metal: newTextureWithDescriptor(resolve) failed");
        }
    }

    return std::make_unique<MetalRenderTarget>(color, resolve, desc.extent, desc.format,
                                               desc.sample_count, false, desc.filter, desc.address);
}

void MetalDevice::wait_idle() {
    // Teardown and debug only, never per frame (docs/guides/writing-a-backend.md section 3). The
    // bound is the last value handed out: waiting for it covers every submission made so far.
    const std::uint64_t target = next_value_;
    if (target == 0) {
        return;
    }
    [event_ waitUntilSignaledValue:target timeoutMS:5000];
    retire_.drain();
}

bool MetalDevice::supports_sample_count(rhi::TextureFormat /*format*/,
                                        std::uint32_t sample_count) const {
    if (!rhi::is_supported_sample_count(sample_count)) {
        return false;
    }
    if (sample_count == 1) {
        return true;
    }
    // Metal reports sample-count support per device, not per pixel format: every RHI format is an
    // 8-bit colour format, and `supportsTextureSampleCount:` is the device's answer for all of
    // them. The parameter is unnamed because the body does not read it.
    return [device_ supportsTextureSampleCount:sample_count] == YES;
}

bool MetalDevice::supports_dual_source_blend() const {
    // Dual-source blending needs the second fragment output, which the MSL translation emits only
    // when the shader declares it. Until the pipeline path proves it per device, report false so a
    // caller takes the documented grayscale-coverage fallback rather than getting an undefined
    // blend (RHI 1.5, docs/design/gpu/rhi.md section 4.1).
    return false;
}

bool MetalDevice::supports_texture_mipmaps(rhi::TextureFormat format) const {
    // The chain is built with render passes into each level, so the format only has to be
    // renderable. Every RHI format is an 8-bit colour format, which is.
    //
    // The capability is reported false until those downsample passes are actually recorded: the RHI
    // contract makes this the only authority, and reporting true while `create_texture` builds no
    // chain would hand a caller a texture whose upper levels are undefined. A caller that sees
    // false samples the single level, which is correct (RHI 1.6, docs/design/gpu/rhi.md section
    // 5.2). Flip this to true in the same change that records the chain.
    (void)format;
    return false;
}

// --- factory -----------------------------------------------------------------------------------

bool metal_available() {
    return MTLCreateSystemDefaultDevice() != nil;
}

std::unique_ptr<rhi::Device> create_metal_device(const rhi::DeviceDesc& desc) {
    if (desc.backend != rhi::Backend::Metal) {
        throw std::invalid_argument("metal: create_metal_device needs Backend::Metal");
    }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
        throw std::runtime_error("metal: no Metal device is available");
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil) {
        throw std::runtime_error("metal: newCommandQueue failed");
    }
    // One shared event carries every completion value (section 3.1). It is created with the
    // default (tracked) hazard tracking, per ADR-053 decision 3.
    id<MTLSharedEvent> event = [device newSharedEvent];
    if (event == nil) {
        throw std::runtime_error("metal: newSharedEvent failed");
    }
    return std::make_unique<MetalDevice>(device, queue, event, desc);
}

} // namespace tessera::rendering::metal
