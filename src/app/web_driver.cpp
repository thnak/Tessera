// SPDX-License-Identifier: MIT

#include "tessera/hal/window.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/input_pump.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_text_measurer.hpp"
#include "web_driver.hpp"
#include "web_gpu_stack.hpp"

#include <cstdio>
#include <utility>

namespace tessera::app {

namespace {

bool scene_dirty(const ui::Node& root) {
    return root.subtree_dirty() != ui::Dirty::Clean || root.dirty() != ui::Dirty::Clean;
}

// The headless measurer: the same faces the glyph renderer would draw, with no device involved.
std::shared_ptr<const ui::text::TextMeasurer> load_measurer(const WebAppOptions& options) {
    std::vector<ui::text::FontFace> faces;
    for (const std::string& path : options.font_paths) {
        auto loaded = ui::text::FontFace::load_from_file(path);
        if (!loaded) {
            std::fprintf(stderr, "%s: cannot load font %s: %s\n", options.log_prefix.c_str(),
                         path.c_str(), loaded.error().message.c_str());
            return nullptr;
        }
        faces.push_back(*loaded);
    }
    if (faces.empty()) {
        return nullptr;
    }
    return std::make_shared<ui::text::FontTextMeasurer>(faces);
}

} // namespace

struct WebAppDriver::Impl {
    WebAppOptions options;
    hal::web::WebHost& web;
    ui::Node& root;
    runtime::ManualClock clock;
    runtime::RuntimeLoop loop{clock};
    ui::InputPump pump;
    std::unique_ptr<WebGpuStack> gpu;
    std::unique_ptr<AppHost> host;
    WebAppStats stats;
    bool auto_focus_applied = false;
    bool started = false;
    float device_scale = 1.0F;

    Impl(WebAppOptions opts, hal::web::WebHost& host_ref, ui::Node& scene_root)
        : options(std::move(opts)),
          web(host_ref),
          root(scene_root) {}

    void apply_auto_focus() {
        ui::FocusManager& focus = pump.router().focus_manager();
        focus.set_root(root);
        focus.sync_focus_trap(root);
        if (!auto_focus_applied) {
            auto_focus_applied = true;
            if (focus.apply_auto_focus(root)) {
                loop.mark_render_dirty();
            }
        }
    }

    // The page's size in CSS pixels and its device pixel ratio; the host follows both.
    void follow_metrics() {
        const hal::web::WebSurfaceMetrics& metrics = web.metrics();
        device_scale = metrics.device_pixel_ratio > 0.0F ? metrics.device_pixel_ratio : 1.0F;
        if (metrics.width > 0.0F && metrics.height > 0.0F) {
            host->resize(metrics.width, metrics.height, device_scale);
        }
        if (gpu != nullptr) {
            gpu->set_device_scale(device_scale);
        }
        loop.mark_render_dirty();
    }

    void drain_input() {
        ui::InputEvent event;
        while (web.input_queue().pop(event)) {
            ++stats.events;
            (void)pump.queue().push(event);
        }
        pump.pump(root, loop);
        if (scene_dirty(root)) {
            loop.mark_render_dirty();
        }
    }
};

WebAppDriver::WebAppDriver(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
WebAppDriver::~WebAppDriver() = default;

std::unique_ptr<WebAppDriver> WebAppDriver::create(ui::Node& root,
                                                   ui::style::CompiledStyleSheet sheet,
                                                   WebAppOptions options, hal::web::WebHost& web) {
    auto impl = std::make_unique<Impl>(std::move(options), web, root);
    Impl& s = *impl;
    std::shared_ptr<const ui::text::TextMeasurer> measurer = load_measurer(s.options);
    if (measurer == nullptr) {
        return nullptr;
    }
    s.gpu = WebGpuStack::create(s.options);
    if (s.gpu != nullptr) {
        measurer = s.gpu->measurer();
    }
    HostConfig config;
    const hal::web::WebSurfaceMetrics& metrics = web.metrics();
    if (metrics.width > 0.0F && metrics.height > 0.0F) {
        config.width = metrics.width;
        config.height = metrics.height;
    }
    s.device_scale = metrics.device_pixel_ratio > 0.0F ? metrics.device_pixel_ratio : 1.0F;
    config.dpi_scale = s.device_scale;
    config.text_measurer = std::move(measurer);
    s.host = std::make_unique<AppHost>(root, std::move(sheet), s.loop, config);
    // Overlays see input first: Escape and back dismiss, a press outside closes popovers.
    s.pump.set_input_filter(
        [](void* user, ui::Node&, const ui::InputEvent& event) {
            return static_cast<AppHost*>(user)->handle_overlay_input(event);
        },
        s.host.get());
    if (s.options.scheduler != nullptr) {
        s.host->set_update_scheduler(s.options.scheduler);
    }
    if (s.options.configure_host) {
        s.options.configure_host(*s.host);
    }
    if (s.gpu != nullptr) {
        s.gpu->set_device_scale(s.device_scale);
    }
    // Startup is a frame cause: the scene has no dirty flags until the first layout.
    s.loop.mark_render_dirty();
    return std::unique_ptr<WebAppDriver>(new WebAppDriver(std::move(impl)));
}

hal::web::WebFrameResult WebAppDriver::run_frame(double now_seconds) {
    Impl& s = *impl_;
    ++s.stats.frames;
    // The first callback's timestamp is the page's age, not a frame time: it advances the clock but
    // gives the animations no elapsed time.
    const double step = s.started ? now_seconds - s.clock.now_seconds() : 0.0;
    const double advance = now_seconds - s.clock.now_seconds();
    if (advance > 0.0) {
        s.clock.advance(advance);
    }
    s.started = true;
    if (s.web.take_resize()) {
        s.follow_metrics();
    }
    s.drain_input();

    hal::web::WebFrameResult result;
    const runtime::RuntimeTick tick = s.loop.tick();
    const bool painting = s.gpu != nullptr && !s.gpu->failed();
    if (tick.should_render) {
        if (painting && !s.gpu->has_capacity()) {
            // The GPU is behind: nothing was consumed, so the frame stays owed and a due timer asks
            // for the next callback instead of waiting for a wake that may never come.
            result.timer_wait_seconds = 0.0;
            result.tick = tick;
            result.tick.idle = false;
            return result;
        }
        const HostFrame frame = s.host->begin_frame(step > 0.0 ? step : 0.0, tick.alpha);
        s.apply_auto_focus();
        const bool has_damage = !frame.render.redraw_plan.scissors.empty();
        if (painting && (frame.frame_due || has_damage || frame.render.full_redraw)) {
            if (s.gpu->paint(*s.host, s.web.metrics())) {
                ++s.stats.presents;
                const hal::web::WebSurfaceExtent extent = s.gpu->last_extent();
                std::fprintf(stderr, "%s: canvas frame %u at %ux%u scale %.2f\n",
                             s.options.log_prefix.c_str(), s.stats.presents, extent.width,
                             extent.height, static_cast<double>(s.device_scale));
            }
        }
        s.host->end_frame();
        s.loop.clear_render_dirty();
    }
    result.tick = s.loop.tick();
    return result;
}

bool WebAppDriver::gpu() const {
    return impl_->gpu != nullptr;
}

const WebAppStats& WebAppDriver::stats() const {
    return impl_->stats;
}

AppHost& WebAppDriver::host() {
    return *impl_->host;
}

} // namespace tessera::app
