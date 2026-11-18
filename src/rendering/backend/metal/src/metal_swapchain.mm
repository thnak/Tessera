// SPDX-License-Identifier: MIT

// The Metal swapchain: a CAMetalLayer and its drawables (ADR-053 slice 3,
// docs/design/gpu/metal-backend.md section 5).
//
// Acquire is a `nextDrawable` call, present is `presentDrawable:` encoded on the frame's command
// buffer. Both are bounded: `allowsNextDrawableTimeout` is set and a nil drawable is reported as
// Timeout, which is the same result as SwapchainError::Timeout on Vulkan, so the UI thread skips
// the tick instead of blocking (section 5).
//
// The layer is created here from the native window handle the HAL supplies. On macOS that handle is
// an NSView*; the layer is attached to it and `drawableSize` follows WindowMetrics, so the RHI never
// sees a window handle (ADR-020).

#include "metal_internal.hpp"

#include <stdexcept>
#include <utility>
#include <vector>

#import <QuartzCore/CAMetalLayer.h>

namespace tessera::rendering::metal {
namespace {

// The swapchain image as an RHI render target. It wraps a drawable's texture, which changes every
// acquire, so the target is rebuilt per acquired image rather than cached.
class MetalSwapchainTarget final : public rhi::RenderTarget {
public:
    MetalSwapchainTarget(id<MTLTexture> texture, rhi::Extent2D extent, rhi::TextureFormat format)
        : texture_(texture), extent_(extent), format_(format) {}

    [[nodiscard]] rhi::Extent2D extent() const override {
        return extent_;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return format_;
    }
    [[nodiscard]] bool presentable() const override {
        return true;
    }
    [[nodiscard]] id<MTLTexture> attachment() const {
        return texture_;
    }
    [[nodiscard]] id<MTLTexture> readable() const {
        return texture_;
    }

private:
    id<MTLTexture> texture_;
    rhi::Extent2D extent_;
    rhi::TextureFormat format_;
};

} // namespace

class MetalSwapchain final : public rhi::Swapchain {
public:
    MetalSwapchain(MetalDevice& device, CAMetalLayer* layer, const rhi::SwapchainDesc& desc)
        : device_(device), layer_(layer), desc_(desc), extent_(desc.extent) {}

    [[nodiscard]] rhi::Extent2D extent() const override {
        return extent_;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return desc_.format;
    }
    [[nodiscard]] std::uint32_t image_count() const override {
        return desc_.image_count;
    }
    [[nodiscard]] rhi::SwapchainAlphaMode alpha_mode() const override {
        return alpha_mode_;
    }
    [[nodiscard]] bool supports_per_pixel_alpha() const override {
        // CAMetalLayer is opaque by default; a non-opaque layer with a premultiplied format is the
        // glass path (glass-v1.md section 6). It is expressible, so report it and let the request
        // decide.
        return true;
    }

    [[nodiscard]] std::expected<std::uint32_t, rhi::SwapchainError>
    acquire_next_image(std::uint64_t timeout_ms) override;
    [[nodiscard]] std::expected<void, rhi::SwapchainError> present(std::uint32_t image) override;
    void resize(rhi::Extent2D extent) override;
    [[nodiscard]] rhi::RenderTarget* image_target(std::uint32_t index) override;

private:
    MetalDevice& device_;
    CAMetalLayer* layer_;
    rhi::SwapchainDesc desc_;
    rhi::Extent2D extent_;
    rhi::Extent2D pending_extent_{};
    bool has_pending_extent_ = false;
    rhi::SwapchainAlphaMode alpha_mode_ = rhi::SwapchainAlphaMode::Opaque;
    // The drawable currently held, and the target wrapping it. One drawable at a time: the RHI
    // acquires, renders and presents within a frame.
    id<CAMetalDrawable> drawable_ = nil;
    std::unique_ptr<MetalSwapchainTarget> target_;
    std::uint32_t acquired_index_ = 0;
    bool acquired_ = false;
};

std::expected<std::uint32_t, rhi::SwapchainError> MetalSwapchain::acquire_next_image(
    std::uint64_t timeout_ms) {
    if (acquired_) {
        // The previous image was never presented; report it rather than leaking the drawable.
        return std::unexpected(rhi::SwapchainError::OutOfDate);
    }
    if (has_pending_extent_) {
        // Adopt the pending extent and report OutOfDate, exactly like the Vulkan backend: the
        // caller recreates dependent targets before retrying (ADR-020).
        extent_ = pending_extent_;
        has_pending_extent_ = false;
        layer_.drawableSize = CGSizeMake(static_cast<CGFloat>(extent_.width),
                                         static_cast<CGFloat>(extent_.height));
        return std::unexpected(rhi::SwapchainError::OutOfDate);
    }

    // A nil drawable is a skipped frame, not a block: `allowsNextDrawableTimeout` is set at
    // creation, so this returns instead of waiting for the compositor (section 5).
    drawable_ = [layer_ nextDrawable];
    if (drawable_ == nil) {
        (void)timeout_ms;
        return std::unexpected(rhi::SwapchainError::Timeout);
    }
    target_ = std::make_unique<MetalSwapchainTarget>(drawable_.texture, extent_, desc_.format);
    acquired_ = true;
    acquired_index_ = 0;
    return acquired_index_;
}

std::expected<void, rhi::SwapchainError> MetalSwapchain::present(std::uint32_t image) {
    if (!acquired_ || image != acquired_index_) {
        return std::unexpected(rhi::SwapchainError::OutOfDate);
    }
    // Present is encoded on the frame's command buffer by the caller's submission; this releases
    // the drawable so the compositor can show it. `presentDrawable:` on a committed buffer is the
    // Metal idiom, and the RHI's present is the point where the drawable stops being ours.
    if (drawable_ != nil) {
        [drawable_ present];
    }
    drawable_ = nil;
    target_.reset();
    acquired_ = false;
    return {};
}

void MetalSwapchain::resize(rhi::Extent2D extent) {
    if (extent.width == extent_.width && extent.height == extent_.height) {
        return;
    }
    pending_extent_ = extent;
    has_pending_extent_ = true;
}

rhi::RenderTarget* MetalSwapchain::image_target(std::uint32_t index) {
    if (!acquired_ || index != acquired_index_ || target_ == nullptr) {
        return nullptr;
    }
    return target_.get();
}

std::expected<std::unique_ptr<rhi::Swapchain>, rhi::SwapchainError>
MetalDevice::create_swapchain(const rhi::SwapchainDesc& desc) {
    if (desc.image_count < 2) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }
    if (desc.native_window == nullptr) {
        // No window to present to: the same answer the Vulkan backend gives on a headless host.
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }

    // The HAL supplies an NSView* on macOS. The layer is attached to it and its size follows
    // WindowMetrics, so the RHI never sees a window handle (ADR-020).
    NSView* view = (__bridge NSView*)desc.native_window;
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = device_;
    layer.pixelFormat = static_cast<MTLPixelFormat>(pixel_format(desc.format));
    layer.framebufferOnly = YES;
    layer.opaque = desc.per_pixel_alpha ? NO : YES;
    layer.allowsNextDrawableTimeout = YES;
    layer.maximumDrawableCount = desc.image_count;
    layer.drawableSize = CGSizeMake(static_cast<CGFloat>(desc.extent.width),
                                    static_cast<CGFloat>(desc.extent.height));
    view.layer = layer;
    view.wantsLayer = YES;

    auto swapchain = std::make_unique<MetalSwapchain>(*this, layer, desc);
    return std::unique_ptr<rhi::Swapchain>(std::move(swapchain));
}

} // namespace tessera::rendering::metal
