// SPDX-License-Identifier: MIT
//
// The Android native entry point (issue #189, docs/design/mobile-integration.md section 3.1).
//
// `android_native_app_glue` runs `android_main` on its own thread and delivers lifecycle and input
// events through `android_app`. This file owns the one thing the engine cannot do for itself on
// Android: wait for the OS to hand over an `ANativeWindow`, then hand it to `hal::create_window` as
// `WindowDesc::surface` (the opaque token seam, ADR-020 rule 1).
//
// The loop is deliberately small. It does NOT run the frame loop on the glue thread: `WindowHost`
// is single-threaded and must be created, run and destroyed on the thread that owns the window, so
// the host is created when a surface arrives and destroyed when it goes away. That is also what
// makes `surfaceDestroyed` safe: the host is gone before the window can be reused.
//
// What is not wired yet, each with its own slice:
//   - Input. `AInputEvent` arrives here and is dropped; translating `MotionEvent`/`KeyEvent` into
//     `ui::InputEvent` is docs/design/mobile-integration.md section 3.2.
//   - Insets, cutouts, fold. These need `WindowInsets`/`WindowMetricsCalculator` over JNI
//     (sections 3.5 and 3.6), so the metrics carry the documented zero-inset fallback.
//   - IME and accessibility (sections 3.3 and 3.4).

#include "scene.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <cstdint>
#include <memory>

namespace {

constexpr const char* kLogTag = "tessera";

void log_info(const char* message) {
    __android_log_write(ANDROID_LOG_INFO, kLogTag, message);
}

// The density the platform reports for this display, as the scale factor the HAL expects
// (1.0 = mdpi). `AConfiguration_getDensity` returns a DPI bucket (160 = mdpi), so the scale is
// density / 160.
float display_scale(android_app* app) {
    AConfiguration* config = app->config;
    if (config == nullptr) {
        return 1.0f;
    }
    const int32_t density = AConfiguration_getDensity(config);
    if (density <= 0) {
        return 1.0f;
    }
    return static_cast<float>(density) / 160.0f;
}

// One app instance: the scene and stylesheet the host runs, plus the host itself, which only exists
// while the OS has given us a surface.
struct AppState {
    // The scene outlives the host: the host borrows the root and owns the compiled stylesheet, so
    // the scene is created once and the host is created and destroyed around it as surfaces come
    // and go.
    std::unique_ptr<tessera::android_shell::ShellScene> scene;
    std::unique_ptr<tessera::app::WindowHost> host;
    bool has_focus = false;
};

void on_app_cmd(android_app* app, int32_t command) {
    auto* state = static_cast<AppState*>(app->userData);
    switch (command) {
    case APP_CMD_INIT_WINDOW:
        // The OS handed us a surface. Create the host now, on this thread, because WindowHost must
        // be created and run on the thread that owns the window.
        if (app->window != nullptr && state->host == nullptr) {
            tessera::app::WindowOptions options;
            options.title = "Tessera";
            options.visible = true;
            options.vsync = true;
            // The opaque token seam: the backend interprets this as an ANativeWindow*.
            options.surface.token = app->window;
            options.dpi_scale = display_scale(app);
            log_info("surface acquired; starting the window host");
            // The scene is created once and reused across surface loss: a backgrounded app keeps
            // its state, which is what a user expects when they come back to it.
            if (state->scene == nullptr) {
                state->scene = std::make_unique<tessera::android_shell::ShellScene>();
            }
            state->host = std::make_unique<tessera::app::WindowHost>(
                state->scene->root, tessera::android_shell::shell_stylesheet(), options,
                &state->scene->scheduler);
            if (!state->host->available()) {
                log_info("the window host is unavailable; no frame will be presented");
            }
        }
        break;
    case APP_CMD_TERM_WINDOW:
        // The surface is going away. Destroy the host BEFORE the window can be reused, so no
        // renderer holds a borrowed surface that outlives it. The scene survives, so the app keeps
        // its state across a background/foreground cycle.
        log_info("surface lost; stopping the window host");
        state->host.reset();
        break;
    case APP_CMD_GAINED_FOCUS:
        state->has_focus = true;
        break;
    case APP_CMD_LOST_FOCUS:
        state->has_focus = false;
        break;
    default:
        break;
    }
}

int32_t on_input(android_app* /*app*/, AInputEvent* /*event*/) {
    // Input translation is the next slice (docs/design/mobile-integration.md section 3.2).
    // Returning 0 means "not handled", which lets the platform fall back to its own behaviour
    // instead of swallowing an event the engine cannot use yet.
    return 0;
}

} // namespace

void android_main(android_app* app) {
    AppState state;
    app->userData = &state;
    app->onAppCmd = on_app_cmd;
    app->onInputEvent = on_input;

    log_info("android_main entered");

    // The glue loop: block until the OS has something for us, then dispatch it. `-1` waits forever,
    // which is what an idle app should do (no busy loop, no background GPU work).
    for (;;) {
        int events = 0;
        android_poll_source* source = nullptr;
        // While the app has focus, poll without blocking so input stays responsive; otherwise block
        // until an event arrives, which is what keeps a backgrounded app at zero CPU.
        const int timeout = state.has_focus ? 0 : -1;
        const int result =
            ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source));
        if (result == ALOOPER_POLL_ERROR) {
            log_info("looper error; exiting");
            break;
        }
        if (source != nullptr) {
            source->process(app, source);
        }
        if (app->destroyRequested != 0) {
            log_info("destroy requested; exiting");
            break;
        }
    }

    state.host.reset();
}
