// SPDX-License-Identifier: MIT

// Built only under Emscripten (see CMakeLists.txt).

#include "tessera/rendering/webgpu/webgpu_device.hpp"
#include "web_canvas_surface.hpp"

#include <cstring>
#include <exception>
#include <webgpu/webgpu.h>

extern "C" {
// Implemented by host/library_tessera_web.js: whether the page handed over a WebGPU device.
int tessera_js_has_gpu_device(void);
}

namespace tessera::hal::web {

struct WebCanvasSurface::Handles {
    WGPUInstance instance = nullptr;
    WGPUDevice device = nullptr;
    WGPUSurface surface = nullptr;

    ~Handles() {
        if (surface != nullptr) {
            wgpuSurfaceRelease(surface);
        }
        if (instance != nullptr) {
            wgpuInstanceRelease(instance);
        }
        // The device is the page's and lives as long as the module; the RHI device took its own
        // reference, so nothing here releases it.
    }
};

WebCanvasSurface::WebCanvasSurface(std::unique_ptr<rendering::rhi::Device> device,
                                   std::unique_ptr<Handles> handles)
    : device_(std::move(device)),
      handles_(std::move(handles)) {}

WebCanvasSurface::~WebCanvasSurface() {
    // The RHI device holds its own references; the surface goes first so no texture outlives it.
    if (configured_ && handles_ != nullptr && handles_->surface != nullptr) {
        wgpuSurfaceUnconfigure(handles_->surface);
    }
}

std::unique_ptr<WebCanvasSurface> WebCanvasSurface::create(const char* canvas_selector) {
    // emscripten_webgpu_get_device() aborts when no device was injected, so ask the page first.
    if (canvas_selector == nullptr || tessera_js_has_gpu_device() == 0) {
        return nullptr;
    }
    auto handles = std::make_unique<Handles>();
    handles->device = emscripten_webgpu_get_device();
    if (handles->device == nullptr) {
        return nullptr;
    }
    handles->instance = wgpuCreateInstance(nullptr);
    if (handles->instance == nullptr) {
        return nullptr;
    }

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas =
        WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector = {canvas_selector, std::strlen(canvas_selector)};
    WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &canvas.chain;
    handles->surface = wgpuInstanceCreateSurface(handles->instance, &descriptor);
    if (handles->surface == nullptr) {
        return nullptr;
    }

    // The first listed format is the browser's preferred one. An unknown format (neither 8-bit
    // unorm layout) is refused: the app then runs without a renderer instead of mis-coloring.
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    wgpuSurfaceGetCapabilities(handles->surface, nullptr, &caps);
    WGPUTextureFormat preferred =
        caps.formatCount > 0 ? caps.formats[0] : WGPUTextureFormat_Undefined;
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    rendering::rhi::TextureFormat format;
    if (preferred == WGPUTextureFormat_BGRA8Unorm) {
        format = rendering::rhi::TextureFormat::B8G8R8A8Unorm;
    } else if (preferred == WGPUTextureFormat_RGBA8Unorm) {
        format = rendering::rhi::TextureFormat::R8G8B8A8Unorm;
    } else {
        return nullptr;
    }

    try {
        WGPUQueue queue = wgpuDeviceGetQueue(handles->device);
        std::unique_ptr<rendering::rhi::Device> device =
            rendering::webgpu::create_device(handles->device, queue);
        wgpuQueueRelease(queue);
        auto surface = std::unique_ptr<WebCanvasSurface>(
            new WebCanvasSurface(std::move(device), std::move(handles)));
        surface->format_ = format;
        return surface;
    } catch (const std::exception&) {
        return nullptr;
    }
}

bool WebCanvasSurface::configure(WebSurfaceExtent extent) {
    if (configured_ && extent == extent_) {
        return true;
    }
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.device = handles_->device;
    config.format = format_ == rendering::rhi::TextureFormat::B8G8R8A8Unorm
                        ? WGPUTextureFormat_BGRA8Unorm
                        : WGPUTextureFormat_RGBA8Unorm;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.width = extent.width;
    config.height = extent.height;
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;
    config.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(handles_->surface, &config);
    extent_ = extent;
    configured_ = true;
    return true;
}

std::unique_ptr<rendering::rhi::RenderTarget> WebCanvasSurface::acquire() {
    if (!configured_) {
        return nullptr;
    }
    WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(handles_->surface, &current);
    if (current.texture == nullptr) {
        return nullptr;
    }
    const bool usable = current.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
                        current.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;
    WGPUTextureView view = usable ? wgpuTextureCreateView(current.texture, nullptr) : nullptr;
    // The view holds the texture alive; the canvas owns the image, so this reference is only ours.
    wgpuTextureRelease(current.texture);
    if (view == nullptr) {
        return nullptr;
    }
    try {
        // create_canvas_target takes over the view's one reference.
        return rendering::webgpu::create_canvas_target(view, {extent_.width, extent_.height},
                                                       format_);
    } catch (const std::exception&) {
        wgpuTextureViewRelease(view);
        return nullptr;
    }
}

} // namespace tessera::hal::web
