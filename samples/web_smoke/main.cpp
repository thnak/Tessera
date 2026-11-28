// SPDX-License-Identifier: MIT

// The smallest web app (issue #969, ADR-046): proves the Emscripten toolchain compiles and links
// the web HAL exports, the page glue and `tessera_web_app`, and gives the browser run something
// that counts frame callbacks. It has no renderer; the gallery needs the web app driver (WebGPU
// device, canvas surface) before it can replace this.

#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "web_exports.hpp"

#include <emscripten.h>

namespace {

using namespace tessera;

struct SmokeApp {
    runtime::ManualClock clock;
    runtime::RuntimeLoop loop{clock};
    hal::web::WebHost host{hal::web::js_callbacks()};
    unsigned frames = 0;
    unsigned events = 0;
};

SmokeApp* g_app = nullptr;

hal::web::WebFrameResult run_frame(void* user, double now_seconds) {
    auto* app = static_cast<SmokeApp*>(user);
    ++app->frames;
    const double step = now_seconds - app->clock.now_seconds();
    if (step > 0.0) {
        app->clock.advance(step);
    }
    ui::InputEvent event;
    while (app->host.input_queue().pop(event)) {
        ++app->events;
    }
    hal::web::WebFrameResult result;
    result.tick = app->loop.tick();
    return result;
}

} // namespace

extern "C" {
// Read by the browser check: frames run and events consumed. A static page keeps both flat.
EMSCRIPTEN_KEEPALIVE unsigned tessera_smoke_frames(void) {
    return g_app != nullptr ? g_app->frames : 0U;
}
EMSCRIPTEN_KEEPALIVE unsigned tessera_smoke_events(void) {
    return g_app != nullptr ? g_app->events : 0U;
}
}

int main() {
    static SmokeApp app;
    g_app = &app;
    app.host.set_frame_fn(&run_frame, &app);
    hal::web::attach_global_host(&app.host);
    return 0;
}
