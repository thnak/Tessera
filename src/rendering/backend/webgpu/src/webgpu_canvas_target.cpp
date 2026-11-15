// SPDX-License-Identifier: MIT

// Built only by the wasm-webgpu preset (Emscripten); see webgpu_internal.hpp.
//
// A render target over a **host-owned** texture view: the browser's canvas. `WebGpuRenderTarget`
// allocates its own textures, which is right for an offscreen target but wrong for presentation -
// the canvas image comes from `context.getCurrentTexture()` in JavaScript, changes every frame, and
// must be drawn into rather than replaced. The Wasm host therefore asks the page for the current
// view and wraps it here; everything below the RHI is unchanged, so the paint pass and the batched
// 2D renderer draw into a canvas exactly as they draw into an offscreen target.
//
// Presentation (the swapchain) stays the host's job: `Device::create_swapchain` is still
// `NotSupported`, because a canvas surface is configured by the page, not by the RHI. This type is
// only the "draw here" half.

#include "webgpu_internal.hpp"

#include <stdexcept>
#include <utility>

namespace tessera::rendering::webgpu {
namespace {

// The canvas image, wrapped as an RHI render target. It owns the view it was given (the page hands
// over one reference per frame and never touches it again) and never the texture: the canvas owns
// that, and destroying it here would break the next frame.
class WebGpuCanvasTarget final : public rhi::RenderTarget {
public:
    WebGpuCanvasTarget(rhi::Extent2D extent, rhi::TextureFormat format, WGPUTextureView view)
        : extent_(extent),
          format_(format),
          view_(view) {}

    ~WebGpuCanvasTarget() override {
        if (view_ != nullptr) {
            wgpuTextureViewRelease(view_);
        }
    }

    WebGpuCanvasTarget(const WebGpuCanvasTarget&) = delete;
    WebGpuCanvasTarget& operator=(const WebGpuCanvasTarget&) = delete;

    [[nodiscard]] rhi::Extent2D extent() const override {
        return extent_;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return format_;
    }
    // A canvas image is always 1-sample: WebGPU has no multisampled swapchain, and a 4x target
    // would need a resolve the page would have to own.
    [[nodiscard]] std::uint32_t sample_count() const override {
        return 1;
    }
    // Presentable: the RHI refuses to use it as a copy source or a sampled bind, which is exactly
    // right for a swapchain-style image.
    [[nodiscard]] bool presentable() const override {
        return true;
    }

    [[nodiscard]] WGPUTextureView view() const {
        return view_;
    }
    // A canvas target cannot be sampled or copied from, so it has no resolve image and no sampler;
    // the command list's sampled-target path never reaches this type because `presentable()` is
    // true.
    [[nodiscard]] WGPUTextureView color_view() const {
        return view_;
    }
    [[nodiscard]] WGPUTextureView resolve_view() const {
        return nullptr;
    }
    [[nodiscard]] WGPUTextureView sampled_view() const {
        return view_;
    }

private:
    rhi::Extent2D extent_;
    rhi::TextureFormat format_;
    WGPUTextureView view_ = nullptr;
};

} // namespace

std::unique_ptr<rhi::RenderTarget> create_canvas_target(void* texture_view, rhi::Extent2D extent,
                                                        rhi::TextureFormat format) {
    if (texture_view == nullptr) {
        throw std::invalid_argument("create_canvas_target: a texture view is required");
    }
    if (extent.width == 0 || extent.height == 0) {
        throw std::invalid_argument("create_canvas_target: the extent must be positive");
    }
    // The page creates the view and hands over its reference; this target owns it from here and
    // releases it in its destructor, so one frame is one view and no view is leaked.
    return std::make_unique<WebGpuCanvasTarget>(extent, format,
                                                static_cast<WGPUTextureView>(texture_view));
}

} // namespace tessera::rendering::webgpu
