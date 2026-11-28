// SPDX-License-Identifier: MIT

#pragma once

#include "web_driver.hpp"
#include "web_surface_extent.hpp"

#include <memory>
#include <string_view>

// The GPU half of the web app driver (issue #1023): everything that exists only once the page has
// handed over a WebGPU device. Private to src/app, like window_internal.hpp.

namespace tessera::app {

class WebGpuStack {
public:
    // Null when the page gave no device, or the device refused a pipeline or a font; the driver
    // then runs headless. Never throws.
    [[nodiscard]] static std::unique_ptr<WebGpuStack> create(const WebAppOptions& options);
    ~WebGpuStack();
    WebGpuStack(const WebGpuStack&) = delete;
    WebGpuStack& operator=(const WebGpuStack&) = delete;

    // The face the layout engine measures with: the one the glyph renderer draws (issue #314).
    [[nodiscard]] std::shared_ptr<const ui::text::TextMeasurer> measurer() const;

    // True when a command list slot is free. Polls `is_complete`, never waits (ADR-021).
    [[nodiscard]] bool has_capacity();

    // Re-resolves the text AA policy for a new device pixel ratio (a window moved to another
    // monitor, or browser zoom).
    void set_device_scale(float device_scale);

    // Records and submits one canvas frame of `host`'s scene. False when the frame was dropped
    // (surface lost or outdated, a failed configure); the next resize or visibility wake retries,
    // never a spin. A thrown GPU error is caught here and drops the stack's usefulness: see
    // `failed()`.
    bool paint(AppHost& host, const hal::web::WebSurfaceMetrics& metrics);

    // Set after a GPU exception; the driver then stops painting and the page idles.
    [[nodiscard]] bool failed() const;
    [[nodiscard]] hal::web::WebSurfaceExtent last_extent() const;

private:
    struct Impl;
    explicit WebGpuStack(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::app
