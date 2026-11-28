// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"
#include "web_surface_extent.hpp"

#include <memory>

// The web app driver's GPU half (issue #1013, ADR-046 item 3): the WebGPU device the page requested
// and the canvas surface it draws into, replacing the Vulkan window driver's device and swapchain.
// Emscripten only (it needs the emdawnwebgpu `webgpu.h`); the size rule it applies is
// `backing_extent`, which is unit tested on every lane.
//
// Nothing here blocks: the page requests the adapter and device asynchronously *before* the module
// starts (`Module.preinitializedWebGPUDevice`, set by tessera_web_host.js), so `create` only wraps
// handles that already exist, and a frame is acquire, record, submit with no wait (ADR-021). The
// browser presents the canvas texture itself when the frame's task ends, so there is no present
// call.

namespace tessera::hal::web {

class WebCanvasSurface {
public:
    // Wraps the page's preinitialized device and binds `canvas_selector` (a CSS selector, for
    // example
    // "#tessera-canvas"). Null when the page handed over no device or the canvas cannot be bound;
    // the app then runs without a renderer instead of aborting.
    [[nodiscard]] static std::unique_ptr<WebCanvasSurface> create(const char* canvas_selector);

    ~WebCanvasSurface();
    WebCanvasSurface(const WebCanvasSurface&) = delete;
    WebCanvasSurface& operator=(const WebCanvasSurface&) = delete;

    [[nodiscard]] rendering::rhi::Device& device() {
        return *device_;
    }
    // The browser's preferred canvas format (BGRA8 or RGBA8 depending on the platform), queried
    // once from the surface capabilities. The pipeline and every target use it: any other format
    // makes the browser add a copy, or WebGPU reject the pipeline at draw time.
    [[nodiscard]] rendering::rhi::TextureFormat format() const {
        return format_;
    }
    [[nodiscard]] WebSurfaceExtent extent() const {
        return extent_;
    }

    // (Re)configures the surface at `extent`. A no-op when it is already configured at that size,
    // so the app can call it on every resize event. Returns false when configuration was refused.
    bool configure(WebSurfaceExtent extent);

    // The canvas texture for this frame as an RHI render target (presentable, 1 sample). Null when
    // the surface is unconfigured, lost or outdated; the app skips the frame and retries on the
    // next wake, which a resize or visibility change provides.
    [[nodiscard]] std::unique_ptr<rendering::rhi::RenderTarget> acquire();

private:
    struct Handles;
    WebCanvasSurface(std::unique_ptr<rendering::rhi::Device> device,
                     std::unique_ptr<Handles> handles);

    std::unique_ptr<rendering::rhi::Device> device_;
    std::unique_ptr<Handles> handles_;
    rendering::rhi::TextureFormat format_ = rendering::rhi::TextureFormat::B8G8R8A8Unorm;
    WebSurfaceExtent extent_;
    bool configured_ = false;
};

} // namespace tessera::hal::web
