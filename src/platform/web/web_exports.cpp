// SPDX-License-Identifier: MIT

// Built only with Emscripten. Unverified without emsdk on the authoring machine: the web CI lane is
// the first compile of this file (docs/design/web-target.md section 6).

#include "web_exports.hpp"

#include <emscripten.h>

extern "C" {
// Implemented by host/library_tessera_web.js (linked with --js-library).
void tessera_js_request_frame(void);
void tessera_js_wake_after(double milliseconds);
}

namespace tessera::hal::web {
namespace {

WebHost* g_host = nullptr;

void request_frame_import(void* /*user*/) {
    tessera_js_request_frame();
}

void wake_after_import(void* /*user*/, double seconds) {
    tessera_js_wake_after(seconds * 1000.0);
}

} // namespace

WebHostCallbacks js_callbacks() {
    WebHostCallbacks callbacks;
    callbacks.request_frame = &request_frame_import;
    callbacks.wake_after = &wake_after_import;
    return callbacks;
}

void attach_global_host(WebHost* host) {
    g_host = host;
}

} // namespace tessera::hal::web

namespace web = tessera::hal::web;

extern "C" {

EMSCRIPTEN_KEEPALIVE void tessera_web_pointer(int phase, int pointer_type, unsigned pointer_id,
                                              int primary, int button, float x, float y,
                                              float pressure, unsigned modifiers,
                                              double timestamp_ms) {
    if (web::g_host == nullptr) {
        return;
    }
    web::DomPointerSample sample;
    sample.phase = static_cast<web::DomPointerPhase>(phase);
    sample.type = pointer_type >= 0 && pointer_type <= 2
                      ? static_cast<web::DomPointerType>(pointer_type)
                      : web::DomPointerType::Unknown;
    sample.pointer_id = pointer_id;
    sample.primary = primary != 0;
    sample.button = button;
    sample.x = x;
    sample.y = y;
    sample.pressure = pressure;
    sample.modifiers = modifiers;
    sample.timestamp_ms = timestamp_ms;
    web::g_host->on_pointer(sample);
}

EMSCRIPTEN_KEEPALIVE void tessera_web_wheel(float x, float y, float dx, float dy, int mode,
                                            unsigned modifiers, double timestamp_ms) {
    if (web::g_host == nullptr) {
        return;
    }
    web::DomWheelSample sample;
    sample.x = x;
    sample.y = y;
    sample.delta_x = dx;
    sample.delta_y = dy;
    sample.mode =
        mode >= 0 && mode <= 2 ? static_cast<web::DomWheelMode>(mode) : web::DomWheelMode::Unknown;
    sample.modifiers = modifiers;
    sample.timestamp_ms = timestamp_ms;
    web::g_host->on_wheel(sample);
}

EMSCRIPTEN_KEEPALIVE void tessera_web_key(int down, const char* key, unsigned modifiers, int repeat,
                                          double timestamp_ms) {
    if (web::g_host == nullptr || key == nullptr) {
        return;
    }
    web::DomKeySample sample;
    sample.down = down != 0;
    sample.key = key;
    sample.modifiers = modifiers;
    sample.repeat = repeat != 0;
    sample.timestamp_ms = timestamp_ms;
    web::g_host->on_key(sample);
}

EMSCRIPTEN_KEEPALIVE void tessera_web_focus(int focused, double timestamp_ms) {
    if (web::g_host != nullptr) {
        web::g_host->on_focus(focused != 0, timestamp_ms);
    }
}

EMSCRIPTEN_KEEPALIVE void tessera_web_resize(float width, float height, float device_pixel_ratio) {
    if (web::g_host != nullptr) {
        web::g_host->on_resize({width, height, device_pixel_ratio});
    }
}

EMSCRIPTEN_KEEPALIVE void tessera_web_visibility(int visible) {
    if (web::g_host != nullptr) {
        web::g_host->on_visibility(visible != 0);
    }
}

EMSCRIPTEN_KEEPALIVE void tessera_web_wake(void) {
    if (web::g_host != nullptr) {
        web::g_host->on_wake();
    }
}

EMSCRIPTEN_KEEPALIVE void tessera_web_frame(double timestamp_ms) {
    if (web::g_host != nullptr) {
        web::g_host->on_frame_callback(timestamp_ms);
    }
}

} // extern "C"
