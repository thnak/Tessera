// SPDX-License-Identifier: MIT
//
// gallery on the web (issue #1023, ADR-046): the same generated pages, stylesheets and theme asset
// as the native gallery, driven by the web app driver instead of the Vulkan window driver. The
// page's WebGPU device draws the text, vector and glass stacks into the canvas; `WebHost` turns DOM
// events into the input the scene dispatches and asks for a `requestAnimationFrame` only when
// something changed, so a static page idles with zero frame callbacks.
//
// The page is chosen from the URL query, the way the native switches are: `?page=form`,
// `?theme=instrument-dark`, `?density=compact`, `?text-scale=125`, `?rtl=1`. The CSS, theme and
// font are preloaded into the module's file system at /gallery (see CMakeLists.txt), so the shared
// `load_resources` reads them like the native build reads the repository.
//
// Without a WebGPU device (Node, a browser without WebGPU) the driver runs headless: the scene
// still styles, lays out and dispatches input, and nothing is drawn. `tessera_render_gpu()` says
// which case it is, and scripts/ci/web_smoke.mjs checks both counters and the download budget.

#include "gallery_vector_assets.hpp"
#include "page_table.hpp"
#include "resources.hpp"
#include "web_driver.hpp"
#include "web_exports.hpp"

#include <cstdio>
#include <cstdlib>
#include <emscripten.h>
#include <memory>
#include <optional>
#include <string>

namespace {

using namespace tessera;

struct GalleryApp {
    gallery::Args args;
    gallery::Resources resources;
    // `Node` keeps a view of its class name, so the text outlives the scene.
    std::string classes;
    std::unique_ptr<gallery::AnyScene> scene;
    std::optional<ui::assets::AssetBundle> vector_bundle;
    hal::web::WebHost host{hal::web::js_callbacks()};
    std::unique_ptr<app::WebAppDriver> driver;
};

GalleryApp* g_app = nullptr;

// One URL query value, empty when absent or when there is no `location` (Node).
std::string query(const char* name) {
    const std::string script =
        std::string("(function(){try{var v=new URLSearchParams(location.search).get('") + name +
        "');return v===null?'':v}catch(e){return ''}})()";
    const char* value = emscripten_run_script_string(script.c_str());
    return value != nullptr ? std::string(value) : std::string();
}

gallery::Args args_from_query() {
    gallery::Args args;
    if (const std::string page = query("page"); gallery::is_page(page)) {
        args.page = page;
    }
    if (const std::string theme = query("theme"); !theme.empty()) {
        args.theme = theme;
    }
    if (query("density") == "compact") {
        args.density = "compact";
    }
    if (const std::string scale = query("text-scale"); scale == "125" || scale == "150") {
        args.text_scale = std::atoi(scale.c_str());
    }
    args.rtl = query("rtl") == "1";
    args.fonts.emplace_back(TESSERA_GALLERY_FONT);
    return args;
}

hal::web::WebFrameResult run_frame(void* user, double now_seconds) {
    return static_cast<GalleryApp*>(user)->driver->run_frame(now_seconds);
}

} // namespace

extern "C" {
// Read by the checks (Node and browser): frames run, events consumed, canvas frames submitted, and
// whether a WebGPU device drove the app. A static page keeps all three flat.
EMSCRIPTEN_KEEPALIVE unsigned tessera_smoke_frames(void) {
    return g_app != nullptr && g_app->driver != nullptr ? g_app->driver->stats().frames : 0U;
}
EMSCRIPTEN_KEEPALIVE unsigned tessera_smoke_events(void) {
    return g_app != nullptr && g_app->driver != nullptr ? g_app->driver->stats().events : 0U;
}
EMSCRIPTEN_KEEPALIVE unsigned tessera_render_presents(void) {
    return g_app != nullptr && g_app->driver != nullptr ? g_app->driver->stats().presents : 0U;
}
EMSCRIPTEN_KEEPALIVE int tessera_render_gpu(void) {
    return g_app != nullptr && g_app->driver != nullptr && g_app->driver->gpu() ? 1 : 0;
}
}

int main() {
    static GalleryApp state;
    g_app = &state;
    state.args = args_from_query();
    std::optional<gallery::Resources> resources = gallery::load_resources();
    if (!resources) {
        return 0; // load_resources printed the reason; the page stays blank
    }
    state.resources = std::move(*resources);
    state.classes = gallery::root_classes(state.args);
    state.scene = gallery::make_scene(state.args.page);
    if (state.scene == nullptr) {
        return 0;
    }
    state.scene->root().set_class_name(state.classes);
    state.vector_bundle = gallery::gen::vector_assets::bundle();

    app::WebAppOptions options;
    options.font_paths = state.args.fonts;
    options.vector_assets = &*state.vector_bundle;
    options.scheduler = &state.scene->scheduler();
    options.log_prefix = "web_gallery";
    options.configure_host = [](app::AppHost& host) {
        host.register_embedded_theme(state.resources.theme_blob);
        if (!host.set_theme(state.args.theme)) {
            std::fprintf(stderr, "web_gallery: unknown theme `%s`\n", state.args.theme.c_str());
        }
    };
    state.driver = app::WebAppDriver::create(
        state.scene->root(), gallery::make_sheet(state.resources), std::move(options), state.host);
    if (state.driver == nullptr) {
        std::fprintf(stderr, "web_gallery: the driver could not start (font?)\n");
        return 0;
    }
    state.host.set_frame_fn(&run_frame, &state);
    hal::web::attach_global_host(&state.host);
    return 0;
}
