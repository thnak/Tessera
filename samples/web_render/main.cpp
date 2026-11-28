// SPDX-License-Identifier: MIT

// The minimal renderer-backed web app (issue #1013, ADR-046): the web app driver end to end. The
// page's WebGPU device and canvas surface (`WebCanvasSurface`) replace the Vulkan window driver,
// the `WebHost` frame callback replaces the window loop, and the batched 2D renderer draws a card
// with a pointer-pressable button into the canvas. It paints only when something changed, so a
// static page holds no frame callback and records no GPU work.
//
// Without a WebGPU device (Node, a browser without WebGPU) it runs the same host contract and draws
// nothing; `tessera_render_gpu()` says which case it is, and scripts/ci/web_smoke.mjs checks it.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "web_canvas_surface.hpp"
#include "web_exports.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <emscripten.h>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace tessera;
namespace rhi = rendering::rhi;

rhi::PipelineDesc batched2d_pipeline_desc(rhi::TextureFormat format) {
    rhi::PipelineDesc desc;
    desc.vertex_layout.stride = rendering::gpu::kVertex2DStride;
    desc.vertex_layout.attributes = rendering::gpu::kVertex2DAttributes;
    desc.vertex_layout.attribute_count = rendering::gpu::kVertex2DAttributeCount;
    desc.color_format = format;
    desc.sample_count = 1;
    desc.blend = rhi::BlendMode::Alpha;
    desc.push_constant_size =
        static_cast<std::uint32_t>(std::max(sizeof(rendering::gpu::Batched2DPushConstants),
                                            sizeof(rendering::gpu::GlassPushConstants)));
    desc.storage_buffer = true;
    // The logical shader name: WebGPU resolves it to the embedded WGSL twin (ADR-049).
    desc.shader_id = rendering::gpu::kBatched2dShaderId;
    return desc;
}

// Everything the renderer borrows, created once the device exists.
struct Gpu {
    std::unique_ptr<hal::web::WebCanvasSurface> surface;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Texture> white;
    std::unique_ptr<rendering::gpu::Batched2DRenderer> renderer;
    // A WebGPU command list is single-use (its encoder is finished at submit), so each frame
    // records a fresh one. Submitted lists are kept until the GPU reports them complete, which is a
    // poll and never a wait (ADR-021), and at most kMaxInFlight are outstanding: the double
    // buffering of the window driver.
    static constexpr std::size_t kMaxInFlight = 2;
    std::vector<std::unique_ptr<rhi::CommandList>> in_flight;

    // Members are released in reverse dependency order; a plain `= {}` would assign them in
    // declaration order and drop the device before the renderer that borrows it.
    void reset() {
        in_flight.clear();
        renderer.reset();
        white.reset();
        pipeline.reset();
        surface.reset();
    }
};

struct App {
    runtime::ManualClock clock;
    runtime::RuntimeLoop loop{clock};
    hal::web::WebHost host{hal::web::js_callbacks()};
    Gpu gpu;
    bool dirty = true;
    bool pressed = false;
    bool hover = false;
    unsigned frames = 0;
    unsigned events = 0;
    unsigned presents = 0;
};

App* g_app = nullptr;

// The card is laid out in CSS pixels at the top left; the button sits inside it.
constexpr rhi::Rect kButton{24.0F, 24.0F, 160.0F, 48.0F};

bool in_button(float x, float y) {
    return x >= kButton.x && x < kButton.x + kButton.width && y >= kButton.y &&
           y < kButton.y + kButton.height;
}

bool create_gpu(App& app) {
    app.gpu.surface = hal::web::WebCanvasSurface::create("#tessera-canvas");
    if (app.gpu.surface == nullptr) {
        return false;
    }
    rhi::Device& device = app.gpu.surface->device();
    app.gpu.pipeline = device.create_pipeline(batched2d_pipeline_desc(app.gpu.surface->format()));
    const std::uint32_t white_pixel = 0xFFFFFFFFU;
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    white_desc.pixels = &white_pixel;
    white_desc.row_pitch = 4;
    app.gpu.white = device.create_texture(white_desc);
    app.gpu.in_flight.reserve(Gpu::kMaxInFlight);
    if (app.gpu.pipeline == nullptr || app.gpu.white == nullptr) {
        app.gpu.reset();
        return false;
    }
    app.gpu.renderer = std::make_unique<rendering::gpu::Batched2DRenderer>(
        device, *app.gpu.pipeline, *app.gpu.white,
        rendering::gpu::Batched2DConfig{4096, true, false});
    return true;
}

// True when the frame was handled (drawn, or dropped because the surface is lost); false when the
// in-flight limit is reached and the frame must be retried on the next callback.
bool paint(App& app) {
    Gpu& gpu = app.gpu;
    std::erase_if(gpu.in_flight, [](const auto& list) { return list->is_complete(); });
    if (gpu.in_flight.size() >= Gpu::kMaxInFlight) {
        return false;
    }
    const hal::web::WebSurfaceExtent extent = hal::web::backing_extent(app.host.metrics());
    if (!gpu.surface->configure(extent)) {
        return true;
    }
    std::unique_ptr<rhi::RenderTarget> target = gpu.surface->acquire();
    if (target == nullptr) {
        // Lost or outdated: the next resize or visibility wake retries, never a rAF spin.
        return true;
    }
    rhi::RenderTargetDesc desc;
    desc.extent = {extent.width, extent.height};
    desc.format = gpu.surface->format();
    desc.clear_color = rhi::rgba8(244, 245, 247);
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;

    // CSS pixels in, device pixels out: the same content scale the native window frame applies.
    const float scale =
        app.host.metrics().device_pixel_ratio > 0.0F ? app.host.metrics().device_pixel_ratio : 1.0F;
    rendering::gpu::Batched2DRenderer& renderer = *gpu.renderer;
    std::unique_ptr<rhi::CommandList> commands = gpu.surface->device().create_command_list();
    if (commands == nullptr) {
        return true;
    }
    renderer.begin(*commands, *target, desc);
    renderer.set_transform(rhi::Mat3::scale(scale, scale));
    renderer.push_rounded_rect({8.0F, 8.0F, 320.0F, 96.0F}, rhi::rgba8(255, 255, 255), 12.0F);
    const rhi::Color fill = app.pressed ? rhi::rgba8(30, 64, 175)
                            : app.hover ? rhi::rgba8(59, 130, 246)
                                        : rhi::rgba8(37, 99, 235);
    renderer.push_rounded_rect(kButton, fill, 8.0F);
    renderer.end();
    commands->submit_async();
    gpu.in_flight.push_back(std::move(commands));
    ++app.presents;
    // The browser check (tools/webgpu_browser/run_web_app.mjs) counts these lines.
    std::fprintf(stderr, "web_render: canvas frame %u at %ux%u scale %.2f\n", app.presents,
                 extent.width, extent.height, static_cast<double>(scale));
    return true;
}

hal::web::WebFrameResult run_frame(void* user, double now_seconds) {
    auto* app = static_cast<App*>(user);
    ++app->frames;
    const double step = now_seconds - app->clock.now_seconds();
    if (step > 0.0) {
        app->clock.advance(step);
    }
    ui::InputEvent event;
    while (app->host.input_queue().pop(event)) {
        ++app->events;
        const bool inside = in_button(event.x, event.y);
        const bool was_pressed = app->pressed;
        const bool was_hover = app->hover;
        if (event.type == ui::InputEventType::PointerDown) {
            app->pressed = inside;
        } else if (event.type == ui::InputEventType::PointerUp ||
                   event.type == ui::InputEventType::PointerCancel) {
            app->pressed = false;
        }
        if (event.type == ui::InputEventType::PointerMove) {
            app->hover = inside;
        }
        app->dirty = app->dirty || app->pressed != was_pressed || app->hover != was_hover;
    }
    if (app->host.take_resize()) {
        app->dirty = true;
    }
    hal::web::WebFrameResult result;
    // Cleared even without a device: a frame with nothing to draw must not keep the page awake. A
    // frame that finds every list in flight sets it again below.
    const bool owed = app->dirty;
    app->dirty = false;
    if (app->gpu.surface != nullptr && owed) {
        // Never throw across the FFI boundary (ADR-025): a failed frame drops the renderer and the
        // page keeps running, reporting the reason once.
        try {
            if (!paint(*app)) {
                // The GPU is behind: keep the frame owed and ask for the next callback.
                app->dirty = true;
                result.timer_wait_seconds = 0.0;
            }
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "web_render: frame failed: %s\n", failure.what());
            app->gpu.reset();
        }
    }
    result.tick = app->loop.tick();
    return result;
}

} // namespace

extern "C" {
// Read by the checks (Node and browser): frames run, events consumed, canvas frames submitted, and
// whether a WebGPU device drove the app. A static page keeps all three flat.
EMSCRIPTEN_KEEPALIVE unsigned tessera_smoke_frames(void) {
    return g_app != nullptr ? g_app->frames : 0U;
}
EMSCRIPTEN_KEEPALIVE unsigned tessera_smoke_events(void) {
    return g_app != nullptr ? g_app->events : 0U;
}
EMSCRIPTEN_KEEPALIVE unsigned tessera_render_presents(void) {
    return g_app != nullptr ? g_app->presents : 0U;
}
EMSCRIPTEN_KEEPALIVE int tessera_render_gpu(void) {
    return g_app != nullptr && g_app->gpu.surface != nullptr ? 1 : 0;
}
}

int main() {
    static App app;
    g_app = &app;
    try {
        create_gpu(app);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "web_render: no renderer: %s\n", failure.what());
        app.gpu.reset();
    }
    app.host.set_frame_fn(&run_frame, &app);
    hal::web::attach_global_host(&app.host);
    return 0;
}
